// SPDX-License-Identifier: GPL-3.0-or-later
#ifdef PSFOIL_PS5
#include <sys/socket.h>
#undef SOCK_CLOEXEC
#endif
#include "httplib.h"
#include "download.h"
#include "package.h"
#include "platform.h"
#include "storage.h"
#include <atomic>
#include <condition_variable>
#include <deque>
#include <fcntl.h>
#include <fstream>
#include <map>
#include <limits.h>
#include <mutex>
#include <regex>
#include <set>
#include <stdexcept>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>
namespace {
using json=nlohmann::json;
using psfoil::ensure_directory;
using psfoil::require_space;
std::mutex lock;
std::condition_variable changed;
std::deque<DownloadSource> pending;
std::map<std::string,json> jobs;
std::vector<std::string> job_order;
struct Batch {std::string title,base_path;std::set<std::string> remaining,outputs;bool failed=false;};
std::map<std::string,Batch> batches;
std::map<std::string,bool> cancellations;
std::thread worker;
bool stopping=false;
void check(const std::string& id) {std::lock_guard<std::mutex> guard(lock);if(stopping||cancellations[id]) throw std::runtime_error("Cancelled");}
void update(const std::string& id,const json& fields) {std::lock_guard<std::mutex> guard(lock);jobs[id].update(fields);}
bool terminal(const json& job) {const auto status=job.value("status",std::string());return status=="completed"||status=="failed"||status=="cancelled";}
void finish_batch(const DownloadSource& source,bool success,bool base=false,const std::string& destination="") {
    std::string ready;
    {std::lock_guard<std::mutex> guard(lock);auto found=batches.find(source.batch_id);if(found==batches.end()) return;
        auto& batch=found->second;batch.remaining.erase(source.id);batch.failed|=!success;
        if(success) {batch.outputs.insert(destination);if(base) batch.base_path=destination;}
        if(batch.remaining.empty()) {
            struct stat file{};bool present=!batch.base_path.empty()&&stat(batch.base_path.c_str(),&file)==0&&S_ISREG(file.st_mode);
            for(const auto& path:batch.outputs) present=present&&stat(path.c_str(),&file)==0&&S_ISREG(file.st_mode);
            if(!stopping&&!batch.failed&&present) ready=batch.title;
            batches.erase(found);
        }
    }
    if(!ready.empty()) notify(ready+" ready to play");
}
struct Url {std::string origin,path;};
Url url_parts(const std::string& value) {
    std::smatch m;static const std::regex re(R"(^(https?://[A-Za-z0-9.\-]+(?::[0-9]{1,5})?)(/[^\r\n#]*)?$)");
    if(!std::regex_match(value,m,re)) throw std::runtime_error("Unsupported download URL");
    return {m[1],m[2].matched?m[2].str():"/"};
}
uint64_t transfer(const DownloadSource& source,const std::string& path) {
    std::string next=source.url;auto original=url_parts(source.url);
    for(int redirects=0;redirects<=5;redirects++) {
        auto url=url_parts(next);httplib::Client client(url.origin);client.set_connection_timeout(10);client.set_read_timeout(25);client.set_write_timeout(15);
        client.set_follow_location(false);client.set_decompress(false);
#ifdef PSFOIL_PS5
        client.set_ca_cert_path("/system/common/cert/CA_LIST.cer");
#endif
        client.enable_server_certificate_verification(true);
        if(url.origin==source.auth_origin) client.set_basic_auth(source.username,source.password);
        FILE* output=nullptr;uint64_t received=0,total=source.expected_size,last_check=0;
        std::string failure;int status=0;
        auto result=client.Get(url.path,httplib::Headers{{"Accept-Encoding","identity"}},[&](const httplib::Response& response) {
            status=response.status;
            if(status>=300&&status<400) return true;
            if(status!=200) return false;
            try {
                check(source.id);
                const auto length=response.get_header_value("Content-Length");
                if(!length.empty()) {auto amount=std::stoull(length);if(total&&amount!=total) throw std::runtime_error("Download length differs from store catalogue");total=amount;}
                if(total) require_space(source.work,total);
                output=fopen(path.c_str(),"wb");if(!output) throw std::runtime_error("Cannot create download staging file");
                update(source.id,{{"total",total}});return true;
            } catch(const std::exception& e) {failure=e.what();return false;}
        },[&](const char* p,size_t n) {
            if(status>=300&&status<400) return true;
            try {
                check(source.id);
                if(!output) throw std::runtime_error("Download stream was not opened");
                if(n>UINT64_MAX-received || (total&&n>total-received)) throw std::runtime_error("Download exceeds expected length");
                if(fwrite(p,1,n,output)!=n) throw std::runtime_error("Cannot write download (check free space)");
                received+=n;
                if(received-last_check>=16*1024*1024) {require_space(source.work,1024*1024);last_check=received;}
                update(source.id,{{"received",received}});return true;
            } catch(const std::exception& e) {failure=e.what();return false;}
        });
        if(output) {if(fflush(output)!=0||fsync(fileno(output))!=0) failure="Cannot flush downloaded file";if(fclose(output)!=0) failure="Cannot close downloaded file";}
        if(!failure.empty()) throw std::runtime_error(failure);
        if(result && result->status>=300 && result->status<400) {
            auto location=result->get_header_value("Location");if(location.empty()) throw std::runtime_error("Download redirect has no location");
            if(location[0]=='/' && (location.size()==1||location[1]!='/')) location=url.origin+location;
            auto redirected=url_parts(location);
            if(redirected.origin!=original.origin && redirected.origin.rfind("https://",0)!=0) throw std::runtime_error("Refusing insecure cross-server redirect");
            next=location;continue;
        }
        if(status==401||status==403) throw std::runtime_error("Store rejected download credentials");
        if(!result||status!=200) throw std::runtime_error("Download failed or connection interrupted (HTTP "+std::to_string(status)+")");
        if(total&&received!=total) throw std::runtime_error("Incomplete download");
        if(!received) throw std::runtime_error("Store returned an empty file");
        return received;
    }
    throw std::runtime_error("Too many download redirects");
}
std::string safe_name(std::string name) {
    auto bracket=name.find('[');if(bracket!=std::string::npos) name.resize(bracket);
    auto dot=name.find_last_of('.');if(dot!=std::string::npos) {auto ext=name.substr(dot);std::transform(ext.begin(),ext.end(),ext.begin(),[](unsigned char c){return std::tolower(c);});if(ext==".nsp"||ext==".nsz"||ext==".xci") name.resize(dot);}
    for(char& c:name) if(static_cast<unsigned char>(c)<32||std::string("/\\:*?\"<>|").find(c)!=std::string::npos) c='_';
    while(!name.empty()&&(name.back()==' '||name.back()=='.')) name.pop_back();
    if(name.empty()) name="Game";
    if(name.size()>110) {size_t cut=110;while(cut>0&&(static_cast<unsigned char>(name[cut])&0xc0)==0x80) --cut;name.resize(cut);}
    return name;
}
void run(const DownloadSource& source) {
    std::string compressed=source.uploaded_path,converted,verified;
    try {
        check(source.id);ensure_directory(source.root);ensure_directory(source.work);
        if(access(source.keys.c_str(),R_OK)!=0) throw std::runtime_error("ProsperoEden prod.keys is unavailable");
        if(compressed.empty()) compressed=source.work+"/"+source.id+".part";
        converted=source.work+"/"+source.id+".part.nsp";
        auto local_unchanged=[&] {
            struct stat st{};char canonical[PATH_MAX];
            if(lstat(source.local_path.c_str(),&st)!=0||!S_ISREG(st.st_mode)||!realpath(source.local_path.c_str(),canonical)||source.local_canonical!=canonical||uint64_t(st.st_dev)!=source.local_device||uint64_t(st.st_ino)!=source.local_inode||uint64_t(st.st_size)!=source.local_size||int64_t(st.st_mtime)!=source.local_modified) throw std::runtime_error("Installed NSZ changed; refresh the library");
        };
        if(!source.uploaded_path.empty()) {
            update(source.id,{{"received",source.expected_size},{"total",source.expected_size}});
        } else if(source.local_path.empty()) {
            update(source.id,{{"status","downloading"}});const auto downloaded=transfer(source,compressed);update(source.id,{{"received",downloaded},{"total",downloaded}});check(source.id);
        } else {local_unchanged();compressed=source.local_path;update(source.id,{{"operation","conversion"},{"status","converting"},{"total",0}});}
        auto ready=compressed;
        if(!source.local_path.empty()||package_is_nsz(compressed)) {
            update(source.id,{{"status","converting"}});
            decompress_nsz(compressed,converted,[&](uint64_t done,uint64_t total){check(source.id);require_space(source.work,1024*1024);update(source.id,{{"converted",done},{"conversionInputSize",total}});});ready=converted;
            struct stat expanded{};if(stat(converted.c_str(),&expanded)!=0||expanded.st_size<0) throw std::runtime_error("Cannot measure converted NSP");
            update(source.id,{{"converted",expanded.st_size},{"convertedTotal",expanded.st_size}});
        }
        update(source.id,{{"status","inspecting"}});check(source.id);
        const auto content=inspect_package(ready,source.keys);
        if(!source.game_id.empty()&&std::any_of(content.begin(),content.end(),[&](const ContentInfo& c){return c.base_id!=source.game_id;})) throw std::runtime_error("Package belongs to a different game than the catalogue");
        verified=ready;
        bool base=std::any_of(content.begin(),content.end(),[](const ContentInfo& c){return c.kind=="BASE";});
        auto folder=source.root+(source.split?(base?"/roms":"/updates"):"");ensure_directory(folder);
        std::ifstream input(ready,std::ios::binary);char magic[4]{};input.read(magic,4);auto extension=std::string(magic,4)=="PFS0"?".nsp":".xci";
        const auto& first=content.front();auto name=safe_name(source.name)+" ["+first.title_id+"][v"+std::to_string(first.version)+"]["+(content.size()>1?"Bundle":first.kind)+"]"+extension;
        auto destination=source.local_path.empty()?folder+"/"+name:source.local_path.substr(0,source.local_path.size()-4)+".nsp";
        check(source.id);if(!source.local_path.empty()) {
            local_unchanged();int fd=open(ready.c_str(),O_RDWR);if(fd<0) throw std::runtime_error("Cannot open verified NSP for flushing");
            const int flushed=fsync(fd);close(fd);if(flushed!=0) throw std::runtime_error("Cannot flush verified NSP; original NSZ retained");
        }
        psfoil::publish_file(ready,destination,[&]{check(source.id);});
        if(!source.local_path.empty()) {verified.clear();update(source.id,{{"destination",destination}});local_unchanged();if(unlink(source.local_path.c_str())!=0) throw std::runtime_error("NSP saved, but original NSZ could not be removed: "+source.local_path);}
        else if(ready!=compressed) unlink(compressed.c_str());
        update(source.id,{{"status","completed"},{"destination",destination},{"kind",base?"BASE":first.kind},{"titleId",first.title_id},{"version",first.version}});
        finish_batch(source,true,base,destination);
    } catch(const std::exception& e) {
        bool cancelled=std::string(e.what())=="Cancelled";
        const bool retain=!cancelled&&!verified.empty()&&std::string(e.what()).find("Destination file already exists")==std::string::npos;
        if(source.local_path.empty()&&!compressed.empty()&&(!retain||verified!=compressed)) unlink(compressed.c_str());
        if(!converted.empty()&&(!retain||verified!=converted)) unlink(converted.c_str());
        if(retain) update(source.id,{{"stagedPath",verified}});
        update(source.id,{{"status",cancelled?"cancelled":"failed"},{"error",e.what()}});
        finish_batch(source,false);
    }
}
}
void downloads_start() {
    std::lock_guard<std::mutex> guard(lock);stopping=false;
    worker=std::thread([] {
        while(true) {
            DownloadSource source;
            {std::unique_lock<std::mutex> guard(lock);changed.wait(guard,[]{return stopping||!pending.empty();});if(stopping) break;source=pending.front();pending.pop_front();}
            run(source);
        }
    });
}
void downloads_request_stop() {
    {std::lock_guard<std::mutex> guard(lock);stopping=true;for(auto& source:pending) {jobs[source.id]["status"]="cancelled";if(!source.uploaded_path.empty()) unlink(source.uploaded_path.c_str());}pending.clear();batches.clear();}
    changed.notify_all();
}
void downloads_shutdown() {
    downloads_request_stop();
    if(worker.joinable()) worker.join();
}
void download_enqueue(const DownloadSource& source) {
    download_enqueue_batch({source},"");
}
void download_enqueue_batch(const std::vector<DownloadSource>& sources,const std::string& title,const std::string& base_path) {
    std::lock_guard<std::mutex> guard(lock);
    const bool on_disk=std::all_of(sources.begin(),sources.end(),[](const DownloadSource& s){return !s.local_path.empty()||!s.uploaded_path.empty();});
    if(stopping||sources.empty()||pending.size()+sources.size()>(on_disk?500:50)||jobs.size()+sources.size()>500) throw std::runtime_error("Download queue is full or shutting down; clear finished history before retrying");
    std::set<std::string> urls,ids;Batch batch;batch.title=title;batch.base_path=base_path;
    for(const auto& source:sources) {
        if(!urls.insert(source.url).second||!ids.insert(source.id).second||jobs.count(source.id)) throw std::runtime_error("Duplicate file in download request");
        if(!title.empty()&&(source.batch_id.empty()||source.batch_id!=sources.front().batch_id||batches.count(source.batch_id))) throw std::runtime_error("Invalid game download batch");
        for(const auto& item:jobs) if(item.second.value("sourceUrl",std::string())==source.url&&!terminal(item.second)) throw std::runtime_error("This file is already queued");
        batch.remaining.insert(source.id);
    }
    if(!title.empty()) batches.emplace(sources.front().batch_id,std::move(batch));
    if(title.empty()) for(const auto& source:sources) if(!source.batch_title.empty()) {
        auto& local=batches[source.batch_id];local.title=source.batch_title;local.base_path=source.batch_base;local.remaining.insert(source.id);
    }
    for(const auto& source:sources) {
        jobs[source.id]={{"id",source.id},{"name",source.name},{"status","queued"},{"received",0},{"total",source.local_path.empty()?source.expected_size:0},{"operation",!source.uploaded_path.empty()?"upload":source.local_path.empty()?"download":"conversion"},{"batchId",source.batch_id},{"sourceUrl",source.url}};
        job_order.push_back(source.id);cancellations[source.id]=false;pending.push_back(source);
    }
    changed.notify_one();
}
size_t download_clear(const std::string& id) {
    std::lock_guard<std::mutex> guard(lock);size_t count=0;
    for(auto it=jobs.begin();it!=jobs.end();) {
        if((id.empty()||it->first==id)&&terminal(it->second)) {cancellations.erase(it->first);it=jobs.erase(it);++count;}
        else ++it;
    }
    job_order.erase(std::remove_if(job_order.begin(),job_order.end(),[](const std::string& item){return !jobs.count(item);}),job_order.end());
    return count;
}
nlohmann::json download_list() {
    std::lock_guard<std::mutex> guard(lock);json result=json::array();for(auto it=job_order.rbegin();it!=job_order.rend();++it) {auto value=jobs.at(*it);value.erase("sourceUrl");result.push_back(value);}return result;
}
bool download_cancel(const std::string& id) {
    std::lock_guard<std::mutex> guard(lock);if(!jobs.count(id)) return false;cancellations[id]=true;return true;
}
