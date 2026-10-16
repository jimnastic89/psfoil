// SPDX-License-Identifier: GPL-3.0-or-later
#ifdef PSFOIL_PS5
#include <sys/socket.h>
#undef SOCK_CLOEXEC
#endif
#include "httplib.h"
#include "upload.h"
#include "download.h"
#include "storage.h"
#include <openssl/rand.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <fcntl.h>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <unistd.h>

namespace {
using json=nlohmann::json;
using Clock=std::chrono::steady_clock;
constexpr uint64_t MAX_FILE_SIZE=128ULL*1024*1024*1024;
struct Session {
    std::mutex mutex;
    std::string id,name,path,root,work,keys;
    uint64_t size=0,received=0;
    bool split=true,handed_off=false,cancelled=false;
    int fd=-1;
    Clock::time_point touched=Clock::now();
    ~Session() {if(fd>=0) close(fd);if(!handed_off&&!path.empty()) unlink(path.c_str());}
};
std::mutex sessions_mutex;
std::map<std::string,std::shared_ptr<Session>> sessions;
std::atomic<bool> stopping{false};
void reply(httplib::Response& res,const json& body,int status=200) {res.status=status;res.set_content(body.dump(),"application/json");}
std::string random_id() {
    unsigned char bytes[16];if(RAND_bytes(bytes,sizeof(bytes))!=1) throw std::runtime_error("Cannot create upload ID");
    const char* hex="0123456789abcdef";std::string result;
    for(auto byte:bytes) {result+=hex[byte>>4];result+=hex[byte&15];}return result;
}
std::shared_ptr<Session> find(const std::string& id) {
    std::lock_guard<std::mutex> guard(sessions_mutex);
    const auto found=sessions.find(id);
    if(stopping||found==sessions.end()) throw std::runtime_error("Upload expired or cancelled; start this file again");
    return found->second;
}
void remove(const std::string& id) {std::lock_guard<std::mutex> guard(sessions_mutex);sessions.erase(id);}
uint64_t number(const std::string& value) {
    if(value.empty()||value.size()>20||value.find_first_not_of("0123456789")!=std::string::npos) throw std::runtime_error("Invalid upload offset or length");
    return std::stoull(value);
}
void active(const Session& session) {
    if(stopping||session.cancelled||session.handed_off) throw std::runtime_error("Upload cancelled or already submitted");
}
}

void uploads_routes(httplib::Server& server,std::function<json()> locations) {
    stopping=false;
    server.Post("/api/uploads",[locations](const auto& req,auto& res) {
        if(stopping) throw std::runtime_error("Payload is stopping");
        const json value=json::parse(req.body);auto name=value.value("name",std::string());
        const auto slash=name.find_last_of("/\\");if(slash!=std::string::npos) name=name.substr(slash+1);
        if(name.size()<5||name.size()>512||name.find_first_of("\r\n")!=std::string::npos) throw std::runtime_error("Choose an NSP, NSZ or XCI file");
        auto extension=name.substr(name.size()-4);std::transform(extension.begin(),extension.end(),extension.begin(),[](unsigned char c){return std::tolower(c);});
        if(extension!=".nsp"&&extension!=".nsz"&&extension!=".xci") throw std::runtime_error("Only NSP, NSZ and XCI files can be uploaded");
        if(!value.contains("size")||!value["size"].is_number_integer()||value["size"].get<int64_t>()<=0||value["size"].get<uint64_t>()>MAX_FILE_SIZE) throw std::runtime_error("Upload size must be between 1 byte and 128 GB");
        const auto paths=locations();if(paths.contains("error")) throw std::runtime_error("ProsperoEden configuration is unavailable");
        auto session=std::make_shared<Session>();session->id=random_id();session->name=name;session->size=value["size"].get<uint64_t>();session->root=paths.at("downloadRoot");session->work=paths.at("work");session->keys=paths.at("keys");session->split=paths.value("splitDownloads",true);
        if(access(session->keys.c_str(),R_OK)!=0) throw std::runtime_error("ProsperoEden prod.keys is unavailable");
        psfoil::ensure_directory(session->root);psfoil::ensure_directory(session->work);psfoil::require_space(session->work,session->size);
        session->path=session->work+"/"+session->id+".upload";
        session->fd=open(session->path.c_str(),O_CREAT|O_EXCL|O_RDWR,0600);if(session->fd<0) {session->path.clear();throw std::runtime_error("Cannot create upload staging file");}
        {std::lock_guard<std::mutex> guard(sessions_mutex);
            for(auto it=sessions.begin();it!=sessions.end();) {
                const auto previous=it->second;
                std::unique_lock<std::mutex> item(previous->mutex,std::try_to_lock);
                if(item.owns_lock()&&Clock::now()-previous->touched>std::chrono::hours(1)) it=sessions.erase(it);else ++it;
            }
            if(stopping||sessions.size()>=8) throw std::runtime_error("Too many active uploads; finish or cancel an upload first");
            sessions.emplace(session->id,session);
        }
        reply(res,{{"id",session->id},{"chunkSize",UPLOAD_CHUNK_SIZE}},201);
    });
    server.Put(R"(/api/uploads/([a-f0-9]{32}))",[](const httplib::Request& req,httplib::Response& res,const httplib::ContentReader& reader) {
        auto session=find(req.matches[1]);std::lock_guard<std::mutex> guard(session->mutex);active(*session);
        if(req.get_header_value("Content-Encoding")!=""||req.has_header("Transfer-Encoding")) throw std::runtime_error("Send upload chunks with an explicit length and no content encoding");
        const auto offset=number(req.get_header_value("X-Upload-Offset")),length=number(req.get_header_value("Content-Length"));
        if(offset!=session->received||length==0||length>UPLOAD_CHUNK_SIZE||length>session->size-session->received) throw std::runtime_error("Unexpected upload offset or chunk length");
        psfoil::require_space(session->work,length);uint64_t written=0;
        if(lseek(session->fd,static_cast<off_t>(offset),SEEK_SET)<0) throw std::runtime_error("Cannot seek upload staging file");
        const bool ok=reader([&](const char* data,size_t count) {
            if(stopping||count>length-written) return false;
            size_t position=0;while(position<count) {const auto n=write(session->fd,data+position,count-position);if(n<=0) return false;position+=static_cast<size_t>(n);}
            written+=count;return true;
        });
        if(!ok||written!=length) {if(ftruncate(session->fd,static_cast<off_t>(offset))!=0) {session->cancelled=true;throw std::runtime_error("Cannot roll back interrupted upload; cancel this file");}throw std::runtime_error("Upload interrupted; retry or cancel this file");}
        session->received+=written;session->touched=Clock::now();reply(res,{{"received",session->received},{"total",session->size}});
    });
    server.Post(R"(/api/uploads/([a-f0-9]{32})/commit)",[](const auto& req,auto& res) {
        const std::string id=req.matches[1];auto session=find(id);
        {std::lock_guard<std::mutex> guard(session->mutex);active(*session);
            if(session->received!=session->size) throw std::runtime_error("Upload is incomplete");
            if(fsync(session->fd)!=0) throw std::runtime_error("Cannot flush uploaded file");
            DownloadSource source;source.id=id;source.name=session->name;source.url="upload:"+id;source.uploaded_path=session->path;source.root=session->root;source.work=session->work;source.keys=session->keys;source.split=session->split;source.expected_size=session->size;
            download_enqueue(source);session->handed_off=true;
        }
        remove(id);reply(res,{{"id",id},{"queued",true}},202);
    });
    server.Post(R"(/api/uploads/([a-f0-9]{32})/cancel)",[](const auto& req,auto& res) {
        const std::string id=req.matches[1];auto session=find(id);
        {std::lock_guard<std::mutex> guard(session->mutex);if(session->handed_off) throw std::runtime_error("Upload already submitted");session->cancelled=true;}
        remove(id);reply(res,{{"cancelled",true}});
    });
}
void uploads_shutdown() {stopping=true;std::lock_guard<std::mutex> guard(sessions_mutex);sessions.clear();}
