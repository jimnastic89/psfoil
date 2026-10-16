// SPDX-License-Identifier: GPL-3.0-or-later
#ifdef PSFOIL_PS5
#include <sys/socket.h>
// PS5 exports accept(), but the SDK's FreeBSD headers also advertise accept4().
// Use httplib's portable accept + FD_CLOEXEC fallback on this target.
#undef SOCK_CLOEXEC
#endif
#include "httplib.h"
#include "json.hpp"
#include "assets.h"
#include "platform.h"
#include "download.h"
#include "package.h"
#include "upload.h"
#include <dirent.h>
#include <openssl/rand.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <limits.h>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <mutex>
#include <regex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <sys/stat.h>
#include <thread>
#include <ifaddrs.h>
#include <arpa/inet.h>
#include <unistd.h>
using json=nlohmann::json;
static constexpr const char* VERSION=app_version;
static constexpr size_t CATALOG_LIMIT=48*1024*1024;
static std::mutex settings_lock, metadata_lock, artwork_lock, tile_lock,metadata_refresh_lock,tile_work_lock;
static json settings;
static std::set<std::string> hosts;
static std::string data_dir, tile_status;
static std::map<std::string,std::map<std::string,std::string>> store_artwork;
static std::mutex sources_lock;
static std::map<std::string,std::map<std::string,json>> download_sources;
static std::map<std::string,json> metadata;
static bool metadata_loaded=false;
static std::atomic<bool> metadata_loading{true};
static std::atomic<bool> quit_requested{false};
static std::mutex library_lock;
static json known_titles=json::object();
static std::map<std::string,std::pair<std::string,json>> library_files;
struct LocalFile {std::string path,canonical;dev_t device;ino_t inode;off_t size;time_t modified;};
static std::map<std::string,LocalFile> local_files;
static json lan_urls=json::array();

static void migrate_data(const std::string& legacy) {
    struct stat current{};if(lstat(data_dir.c_str(),&current)==0) return;
    if(errno!=ENOENT) throw std::runtime_error("Cannot inspect PSFoil data directory");
    if(lstat(legacy.c_str(),&current)!=0) {if(errno==ENOENT) return;throw std::runtime_error("Cannot inspect previous payload data");}
    if(!S_ISDIR(current.st_mode)||rename(legacy.c_str(),data_dir.c_str())!=0) throw std::runtime_error("Cannot migrate previous payload data; existing settings preserved");
}

static std::string random_hex(size_t n) {
    std::vector<unsigned char> b(n);
    if(RAND_bytes(b.data(),static_cast<int>(n))!=1) throw std::runtime_error("Random source unavailable");
    static const char* digits="0123456789abcdef";
    std::string out;
    for(auto c:b) { out+=digits[c>>4]; out+=digits[c&15]; }
    return out;
}
static json read_json(const std::string& path) {
    std::ifstream f(path); if(!f) throw std::runtime_error("Cannot read "+path);
    return json::parse(f);
}
static void write_settings_file(const std::string& path,const json& value) {
    const auto temp=path+".tmp";
    const std::string bytes=value.dump(2);
    FILE* f=fopen(temp.c_str(),"wb");
    if(!f) throw std::runtime_error("Cannot save settings");
    if(chmod(temp.c_str(),0600)!=0) { fclose(f); unlink(temp.c_str()); throw std::runtime_error("Cannot protect settings"); }
    bool ok=fwrite(bytes.data(),1,bytes.size(),f)==bytes.size();
    if(ok) ok=fflush(f)==0 && fsync(fileno(f))==0;
    if(fclose(f)!=0) ok=false;
    if(!ok || rename(temp.c_str(),path.c_str())!=0) { unlink(temp.c_str()); throw std::runtime_error("Cannot commit settings"); }
}
static void save_settings(const json& value) {
    write_settings_file(data_dir+"/settings.json",value);
    try {write_settings_file(data_dir+"/settings.json.bak",value);}
    catch(const std::exception& e) {notify(std::string("PSFoil settings backup: ")+e.what());}
}
static void load_settings() {
    const auto path=data_dir+"/settings.json";
    // Open directly: an access() probe must never cause existing settings to be replaced.
    errno=0;FILE* probe=fopen(path.c_str(),"rb");const int error=errno;
    if(probe) fclose(probe);
    try {
        if(!probe) throw std::runtime_error("Cannot open settings");
        auto value=read_json(path);
        if(!value.is_object()||!value.contains("stores")||!value["stores"].is_array()) throw std::runtime_error("Invalid settings");
        settings=std::move(value);
        try {write_settings_file(path+".bak",settings);} catch(const std::exception& e) {notify(std::string("PSFoil settings backup: ")+e.what());}
        return;
    } catch(...) {
        try {
            auto backup=read_json(path+".bak");
            if(!backup.is_object()||!backup.contains("stores")||!backup["stores"].is_array()) throw std::runtime_error("Invalid backup");
            settings=std::move(backup);
            notify("PSFoil recovered saved settings from backup");return;
        } catch(...) {}
        if(probe||error!=ENOENT) throw std::runtime_error("Cannot load "+path+" or its backup; existing settings have been preserved");
        // Leave defaults in memory until the user explicitly saves them.
    }
}
static std::string text(const json& j,const char* field,const std::string& fallback="") {
    return j.contains(field) && j[field].is_string()?j[field].get<std::string>():fallback;
}
struct Url { std::string origin,path; };
static Url parse_url(const std::string& value) {
    std::smatch m;
    static const std::regex pattern(R"(^(https?://[A-Za-z0-9.\-]+(?::[0-9]{1,5})?)(/[^\r\n#]*)?$)");
    if(value.size()>2048 || !std::regex_match(value,m,pattern)) throw std::runtime_error("Use an HTTP/HTTPS URL without credentials, query-only paths, or fragments");
    return {m[1],m[2].matched?m[2].str():"/"};
}
static json fetch_json(const std::string& url,const json& store=json::object()) {
    auto u=parse_url(url);
    httplib::Client client(u.origin);
    client.set_connection_timeout(8); client.set_read_timeout(25); client.set_write_timeout(15);
    client.set_follow_location(false);
#ifdef PSFOIL_PS5
    client.set_ca_cert_path("/system/common/cert/CA_LIST.cer");
#endif
    client.enable_server_certificate_verification(true);
    const auto username=text(store,"username"),password=text(store,"password");
    if(!username.empty()) client.set_basic_auth(username,password);
    std::string body;
    bool oversized=false;
    const auto result=client.Get(u.path,httplib::Headers{{"Accept","application/json"}},
        [&](const char* p,size_t n) {
            if(quit_requested) return false;
            if(n>CATALOG_LIMIT-body.size()) {oversized=true;return false;}
            body.append(p,n); return true;
        });
    if(oversized) throw std::runtime_error("Catalogue exceeds 48 MiB limit");
    if(!result) throw std::runtime_error("Store connection failed: "+httplib::to_string(result.error()));
    if(result->status==401 || result->status==403) throw std::runtime_error("Store rejected these credentials or account permissions");
    if(result->status!=200) throw std::runtime_error("Store returned HTTP "+std::to_string(result->status));
    if(body.compare(0,7,"TINFOIL")==0) throw std::runtime_error("Encrypted Tinfoil catalogue unsupported. For Ownfoil, select Ownfoil API as the store type.");
    auto parsed=json::parse(body,nullptr,false);
    if(parsed.is_discarded()) throw std::runtime_error("Store did not return a JSON catalogue");
    if(parsed.contains("errors") || parsed.contains("error")) throw std::runtime_error("Store returned a catalogue/API error; check type and permissions");
    return parsed;
}
static std::string encode(const std::string& s) {
    static const char* h="0123456789ABCDEF"; std::string out;
    for(unsigned char c:s) {
        if(std::isalnum(c)||c=='-'||c=='_'||c=='.'||c=='~') out+=c;
        else {out+='%';out+=h[c>>4];out+=h[c&15];}
    } return out;
}
static std::string decode(const std::string& s) {
    std::string out;
    for(size_t i=0;i<s.size();i++) {
        if(s[i]=='%' && i+2<s.size() && std::isxdigit(static_cast<unsigned char>(s[i+1])) && std::isxdigit(static_cast<unsigned char>(s[i+2]))) {
            auto c=static_cast<char>(std::stoul(s.substr(i+1,2),nullptr,16));out+=static_cast<unsigned char>(c)<32?' ':c;i+=2;
        } else out+=s[i];
    }return out;
}
static std::string resolve_url(const std::string& base,std::string relative) {
    relative=relative.substr(0,relative.find('#'));
    if(relative.rfind("http://",0)==0||relative.rfind("https://",0)==0) {parse_url(relative);return relative;}
    auto url=parse_url(base);
    if(relative.rfind("//",0)==0) throw std::runtime_error("Scheme-relative download URLs are unsupported");
    return url.origin+(!relative.empty()&&relative[0]=='/'?relative:url.path.substr(0,url.path.find_last_of('/')+1)+relative);
}
static std::string entry_id(const std::string& value) {
    unsigned char hash[EVP_MAX_MD_SIZE];unsigned n=0;
    if(EVP_Digest(value.data(),value.size(),hash,&n,EVP_sha256(),nullptr)!=1) throw std::runtime_error("Cannot identify download");
    static const char* chars="0123456789abcdef";std::string id;for(size_t i=0;i<12;i++){id+=chars[hash[i]>>4];id+=chars[hash[i]&15];}return id;
}
static json public_settings() {
    std::lock_guard<std::mutex> g(settings_lock);
    auto result=settings;
    for(auto& store:result["stores"]) { store["hasPassword"]=!text(store,"password").empty();store.erase("password"); }
    return result;
}
static void reply(httplib::Response& res,const json& value,int status=200) {
    res.status=status;res.set_content(value.dump(),"application/json");
}
static bool trusted_request(const httplib::Request& req) {
    auto host=req.get_header_value("Host");
    if(!hosts.count(host)) return false;
    auto origin=req.get_header_value("Origin");
    return origin.empty() || origin=="http://"+host;
}
static json paths() {
    const char* override=getenv("PSFOIL_PROSPERO_CONFIG");
    const std::string path=override?override:"/data/prosperoeden/config/prosperoeden.json";
    try {
        auto config=read_json(path); auto root=text(config,"game_files");
        if(root.empty()||root[0]!='/'||root.find("/../")!=std::string::npos) throw std::runtime_error("game_files must be an absolute console path");
        while(root.size()>1 && root.back()=='/') root.pop_back();
        std::string destination=root;bool split=true;
        {std::lock_guard<std::mutex> guard(settings_lock);auto custom=text(settings,"downloadDirectory");if(!custom.empty()) destination=custom;split=settings.value("splitDownloads",true);}
        return {{"config",path},{"root",root},{"roms",root+"/roms"},{"updates",root+"/updates"},
            {"downloadRoot",destination},{"downloadRoms",destination+(split?"/roms":"")},{"downloadUpdates",destination+(split?"/updates":"")},{"splitDownloads",split},
            {"work",root+"/.psfoil"},{"keys",root+"/keys/prod.keys"},{"keysPresent",access((root+"/keys/prod.keys").c_str(),R_OK)==0}};
    } catch(const std::exception& e) {return {{"config",path},{"error",e.what()}};}
}
static json metadata_status() {
    std::lock_guard<std::mutex> g(metadata_lock);
    return {{"loaded",metadata_loaded},{"loading",metadata_loading.load()},{"titles",metadata.size()}};
}
static std::string upper(std::string s) {
    std::transform(s.begin(),s.end(),s.begin(),[](unsigned char c){return std::toupper(c);});return s;
}
static void import_metadata(const json& records,const std::string& region="",const std::string& language="") {
    if(!records.is_object()) throw std::runtime_error("Invalid titledb format");
    std::map<std::string,json> next;
    for(const auto& record:records) {
        if(!record.is_object()) continue;
        auto id=upper(text(record,"id"));
        if(id.size()!=16) continue;
        next[id]={{"id",id},{"name",text(record,"name")},{"iconUrl",text(record,"iconUrl")},
            {"description",text(record,"description")},{"publisher",text(record,"publisher")}};
    }
    if(next.empty()) throw std::runtime_error("No title IDs found in titledb");
    std::unique_lock<std::mutex> settings_guard(settings_lock,std::defer_lock);
    if(!region.empty()) {settings_guard.lock();if(region!=text(settings,"region")||language!=text(settings,"language")) throw std::runtime_error("Metadata region changed; refresh again");}
    std::lock_guard<std::mutex> g(metadata_lock);
    metadata=std::move(next); metadata_loaded=true;
}
static std::string image_mime(const std::string& bytes) {
    if(bytes.compare(0,8,"\x89PNG\r\n\x1a\n",8)==0) return "image/png";
    if(bytes.size()>=3 && static_cast<unsigned char>(bytes[0])==255 && static_cast<unsigned char>(bytes[1])==216 && static_cast<unsigned char>(bytes[2])==255) return "image/jpeg";
    if(bytes.size()>=12 && bytes.compare(0,4,"RIFF")==0 && bytes.compare(8,4,"WEBP")==0) return "image/webp";
    return "";
}
static bool cached_artwork(const std::string& id,httplib::Response& res) {
    std::ifstream f(data_dir+"/artwork/"+upper(id)+".img",std::ios::binary);
    if(!f) return false;
    f.seekg(0,std::ios::end);auto length=f.tellg();if(length<0||length>5*1024*1024) return false;f.seekg(0);
    std::string bytes(static_cast<size_t>(length),'\0');f.read(bytes.data(),bytes.size());
    auto mime=image_mime(bytes);if(!f||mime.empty()) return false;
    res.set_header("Cache-Control","private, max-age=86400");res.set_content(std::move(bytes),mime);return true;
}
static void cache_artwork(const std::string& id,const std::string& bytes) {
    const auto folder=data_dir+"/artwork";if(mkdir(folder.c_str(),0700)!=0&&errno!=EEXIST) return;
    const auto path=folder+"/"+upper(id)+".img",temp=path+"."+random_hex(4)+".tmp";
    std::ofstream f(temp,std::ios::binary);f.write(bytes.data(),bytes.size());f.close();
    if(!f||rename(temp.c_str(),path.c_str())!=0) unlink(temp.c_str());
}
static void enrich_local(json& item) {
    const auto id=text(item,"titleId");
    {std::lock_guard<std::mutex> guard(metadata_lock);auto it=metadata.find(id);if(it!=metadata.end()) {item["metadata"]=it->second;item["name"]=text(it->second,"name",text(item,"name"));item["metadata"]["sourceIconUrl"]=text(it->second,"iconUrl");item["metadata"]["iconUrl"]="/api/artwork/"+id;}}
    if(known_titles.contains(id)) item["name"]=known_titles[id];
    item["artworkUrl"]="/api/artwork/"+id;
}
static json installed_games() {
    const bool metadata_ready=metadata_status().value("loaded",false);
    const auto locations=paths();if(locations.contains("error")) throw std::runtime_error(text(locations,"error"));
    std::lock_guard<std::mutex> guard(library_lock);
    for(const auto* key:{"root","roms","updates","downloadRoot","downloadRoms","downloadUpdates"}) rmdir((text(locations,key)+"/.psfoil-downloads").c_str());
    json items=json::array(),warnings=json::array();std::set<std::string> visited,seen_files;size_t count=0;local_files.clear();
    std::function<void(const std::string&,int)> scan=[&](const std::string& folder,int depth) {
        if(depth>4||quit_requested||!visited.insert(folder).second) return;
        DIR* directory=opendir(folder.c_str());if(!directory) {if(errno!=ENOENT) warnings.push_back("Cannot read directory: "+folder);return;}
        std::vector<std::string> names;while(auto* entry=readdir(directory)) if(std::string(entry->d_name)==".psfoil-downloads") rmdir((folder+"/.psfoil-downloads").c_str());else if(entry->d_name[0]!='.') names.emplace_back(entry->d_name);closedir(directory);
        for(const auto& name:names) {
            if(quit_requested) return;
            if(count>=10000) {warnings.push_back("Library scan limit reached (10,000 files)");return;}
            const auto path=folder+"/"+name;struct stat st{};if(lstat(path.c_str(),&st)!=0) continue;
            if(S_ISDIR(st.st_mode)) {if(name!="keys") scan(path,depth+1);continue;}
            if(!S_ISREG(st.st_mode)||name.size()<4||!seen_files.insert(path).second) continue;
            const auto ext=upper(name.substr(name.size()-4));if(ext!=".NSP"&&ext!=".XCI"&&ext!=".NSZ") continue;
            ++count;const auto stamp=std::to_string(st.st_size)+":"+std::to_string(st.st_mtime)+":"+text(locations,"keys");
            try {
                auto found=library_files.find(path);
                if(found==library_files.end()||found->second.first!=stamp) {
                    json records=json::array();
                    auto label=decode(name.substr(0,name.size()-4));const auto bracket=label.find('[');if(bracket!=std::string::npos) label.resize(bracket);
                    while(!label.empty()&&label.back()==' ') label.pop_back();
                    if(label.empty()) label=name;
                    for(const auto& info:inspect_package(path,text(locations,"keys"),ext==".NSZ")) records.push_back({{"titleId",upper(info.base_id)},{"appId",upper(info.title_id)},{"kind",info.kind},{"version",info.version},{"name",label},{"size",st.st_size},{"path",path},{"format",ext.substr(1)}});
                    library_files[path]={stamp,std::move(records)};found=library_files.find(path);
                }
                const auto file_id=entry_id(path+":"+stamp+":"+std::to_string(st.st_dev)+":"+std::to_string(st.st_ino));
                char canonical[PATH_MAX];if(!realpath(path.c_str(),canonical)) throw std::runtime_error("Cannot resolve installed file path");
                local_files[file_id]={path,canonical,st.st_dev,st.st_ino,st.st_size,st.st_mtime};
                for(auto item:found->second.second) {item["fileId"]=file_id;enrich_local(item);items.push_back(std::move(item));}
            } catch(const std::exception& e) {warnings.push_back(path+": "+e.what());}
        }
    };
    scan(text(locations,"roms"),0);scan(text(locations,"updates"),0);
    scan(text(locations,"downloadRoms"),0);scan(text(locations,"downloadUpdates"),0);
    for(auto it=library_files.begin();it!=library_files.end();) {if(!seen_files.count(it->first)) it=library_files.erase(it);else ++it;}
    return {{"items",items},{"warnings",warnings},{"type","installed"},{"name","Installed games"},{"metadataReady",metadata_ready}};
}
static json delete_installed(const json& value) {
    if(!value.value("confirmed",false)||!value.contains("fileIds")||!value["fileIds"].is_array()||value["fileIds"].empty()||value["fileIds"].size()>10000) throw std::runtime_error("Confirm the installed files to delete");
    // Rescan current configured roots; clients never supply filesystem paths.
    installed_games();std::lock_guard<std::mutex> guard(library_lock);
    std::map<std::string,LocalFile> selected;
    for(const auto& id:value["fileIds"]) {
        if(!id.is_string()) throw std::runtime_error("Invalid installed file ID");
        auto found=local_files.find(id.get<std::string>());if(found==local_files.end()) throw std::runtime_error("Installed file changed or is unavailable; refresh the library");
        const auto& f=found->second;struct stat st{};char canonical[PATH_MAX];
        if(!realpath(f.path.c_str(),canonical)||f.canonical!=canonical||lstat(f.path.c_str(),&st)!=0||!S_ISREG(st.st_mode)||st.st_dev!=f.device||st.st_ino!=f.inode||st.st_size!=f.size||st.st_mtime!=f.modified) throw std::runtime_error("Installed file changed; refresh the library");
        selected.emplace(found->first,f);
    }
    json deleted=json::array(),errors=json::array();
    for(const auto& entry:selected) {
        const auto& path=entry.second.path;
        if(unlink(path.c_str())!=0) errors.push_back({{"path",path},{"error","Cannot delete installed file"}});
        else {deleted.push_back(path);library_files.erase(path);local_files.erase(entry.first);}
    }
    return {{"deleted",deleted},{"errors",errors}};
}
static json convert_installed(const json& value) {
    if(!value.contains("fileIds")||!value["fileIds"].is_array()||value["fileIds"].empty()||value["fileIds"].size()>500) throw std::runtime_error("Choose between 1 and 500 NSZ files");
    const auto library=installed_games(),locations=paths();std::lock_guard<std::mutex> guard(library_lock);
    std::map<std::string,json> items;std::map<std::string,std::string> batch_ids,base_paths;
    for(const auto& item:library["items"]) {items[text(item,"fileId")]=item;if(text(item,"kind")=="BASE"&&text(item,"format")!="NSZ") base_paths[text(item,"titleId")]=text(item,"path");}
    std::set<std::string> seen;std::vector<DownloadSource> jobs;json ids=json::array();
    for(const auto& id:value["fileIds"]) {
        if(!id.is_string()||!seen.insert(id.get<std::string>()).second) throw std::runtime_error("Invalid or duplicate installed file ID");
        auto found=local_files.find(id.get<std::string>());if(found==local_files.end()||text(items[id.get<std::string>()],"format")!="NSZ") throw std::runtime_error("Installed NSZ changed or is unavailable; refresh the library");
        const auto& file=found->second;const auto& item=items.at(id.get<std::string>());const auto game=text(item,"titleId");
        if(batch_ids[game].empty()) batch_ids[game]=random_hex(12);
        DownloadSource job;job.id=random_hex(12);job.name=text(item,"name");job.url="local:"+file.canonical;job.local_path=file.path;job.local_canonical=file.canonical;
        job.local_device=file.device;job.local_inode=file.inode;job.local_size=file.size;job.local_modified=file.modified;job.root=file.path.substr(0,file.path.find_last_of('/'));job.work=text(locations,"work");job.keys=text(locations,"keys");job.split=false;job.game_id=game;job.batch_id=batch_ids[game];job.batch_title=text(item,"name");job.batch_base=base_paths[game];
        ids.push_back(job.id);jobs.push_back(std::move(job));
    }
    download_enqueue_batch(jobs,"");return {{"ids",ids},{"games",batch_ids.size()}};
}
// SAX keeps only one upstream record at a time. The regional source is ~86 MiB;
// constructing its complete JSON DOM would need substantially more memory.
class TitleDbReader : public nlohmann::json_sax<json> {
    int depth=0;
    std::string field;
    json record=json::object();
public:
    json index=json::object();
    bool null() override {return true;}
    bool boolean(bool) override {return true;}
    bool number_integer(number_integer_t) override {return true;}
    bool number_unsigned(number_unsigned_t) override {return true;}
    bool number_float(number_float_t,const string_t&) override {return true;}
    bool string(string_t& value) override {
        if(quit_requested) return false;
        if(depth==2 && (field=="id"||field=="name"||field=="iconUrl"||field=="description"||field=="publisher")) record[field]=value;
        return true;
    }
    bool binary(binary_t&) override {return false;}
    bool start_object(std::size_t) override {++depth;if(depth==2) record=json::object();return depth<32;}
    bool key(string_t& value) override {if(depth==2) field=value;return true;}
    bool end_object() override {
        if(depth==2) {
            auto id=upper(text(record,"id"));
            if(id.size()==16&&id.find_first_not_of("0123456789ABCDEF")==std::string::npos) {record["id"]=id;index[id]=std::move(record);}
        }
        --depth;return true;
    }
    bool start_array(std::size_t) override {++depth;return depth<32;}
    bool end_array() override {--depth;return true;}
    bool parse_error(std::size_t,const std::string&,const nlohmann::detail::exception&) override {return false;}
};
static json fetch_metadata_index(const std::string& region,const std::string& language) {
    const std::string path=data_dir+"/titledb-source.tmp";
    struct Cleanup {std::string path;~Cleanup(){unlink(path.c_str());}} cleanup{path};
    std::ofstream output(path,std::ios::binary|std::ios::trunc);
    if(!output) throw std::runtime_error("Cannot create metadata staging file");
    httplib::Client client("https://raw.githubusercontent.com");
    client.set_connection_timeout(10);client.set_read_timeout(30);client.set_follow_location(false);
#ifdef PSFOIL_PS5
    client.set_ca_cert_path("/system/common/cert/CA_LIST.cer");
#endif
    client.enable_server_certificate_verification(true);
    size_t bytes=0;
    const auto result=client.Get("/blawar/titledb/master/"+region+"."+language+".json",
        httplib::Headers{{"Accept","application/json"},{"Accept-Encoding","gzip"}},[&](const char* p,size_t n) {
            if(quit_requested) return false;
            if(n>256*1024*1024-bytes) return false;
            bytes+=n;output.write(p,static_cast<std::streamsize>(n));return static_cast<bool>(output);
        });
    output.close();
    if(!result) throw std::runtime_error("Metadata transfer failed: "+httplib::to_string(result.error()));
    if(result->status!=200) throw std::runtime_error("titledb region/language unavailable (HTTP "+std::to_string(result->status)+")");
    if(!output) throw std::runtime_error("Cannot write metadata staging file");
    std::ifstream input(path);TitleDbReader parser;
    if(!json::sax_parse(input,&parser)||parser.index.empty()) throw std::runtime_error("Invalid titledb metadata");
    return std::move(parser.index);
}
static json catalogue(const json& store) {
    json result={{"items",json::array()},{"type",store["type"]},{"name",store["name"]}};
    const auto base=text(store,"url"), type=text(store,"type");
    if(type=="ownfoil") {
        auto origin=parse_url(base).origin;
        int page=1,total=0;
        do {
            const std::string query="{ apps(owned:true,pageSize:250,page:"+std::to_string(page)+") { total items { titleId appId appType appVersion displayVersion downloadUrl downloadSize downloadExtension title { name icon { url } banner { url } } } } }";
            auto response=fetch_json(origin+"/api/graphql?query="+encode(query),store);
            auto connection=response.at("data").at("apps");
            total=connection.at("total").get<int>();
            if(total>100000) throw std::runtime_error("Store catalogue exceeds 100,000 entries");
            const auto& items=connection.at("items");
            if(!items.is_array() || (items.empty() && static_cast<int>(result["items"].size())<total)) throw std::runtime_error("Store pagination did not make progress");
            for(auto item:items) {
                auto title=item.value("title",json::object());
                std::string artwork;
                if(title.is_object()) {
                    for(const auto* kind:{"icon","banner"}) {
                        if(title.contains(kind) && title[kind].is_object()) artwork=text(title[kind],"url");
                        if(!artwork.empty()) break;
                    }
                }
                const auto title_id=text(item,"titleId");
                if(!artwork.empty()) {
                    std::lock_guard<std::mutex> guard(artwork_lock);
                    store_artwork[text(store,"id")][title_id]=artwork;
                }
                result["items"].push_back({{"titleId",item["titleId"]},{"appId",item["appId"]},
                    {"kind",item["appType"]},{"version",item["appVersion"]},{"displayVersion",item["displayVersion"]},
                    {"name",title.is_object()?text(title,"name"):text(item,"appId")},
                    {"size",item["downloadSize"]},{"extension",item["downloadExtension"]},
                    {"sourceUrl",item["downloadUrl"].is_string()?resolve_url(base,item["downloadUrl"].get<std::string>()):""},
                    {"artworkUrl",artwork.empty()?"":"/api/stores/"+text(store,"id")+"/artwork/"+title_id}});
            }
            ++page;
        } while(static_cast<int>(result["items"].size())<total);
    } else {
        auto response=fetch_json(base,store);
        if(!response.contains("files")||!response["files"].is_array()) throw std::runtime_error("Tinfoil catalogue has no files array");
        if(response["files"].size()>100000) throw std::runtime_error("Store catalogue exceeds 100,000 entries");
        static const std::regex idpattern(R"(\[([0-9A-Fa-f]{16})\])");
        for(auto file:response["files"]) {
            auto url=file.is_string()?file.get<std::string>():text(file,"url");
            if(url.empty()) continue;
            const auto fragment=url.find('#');
            auto name=decode(fragment==std::string::npos?url.substr(url.find_last_of('/')+1):url.substr(fragment+1));
            std::smatch m;std::string id;
            if(std::regex_search(name,m,idpattern)) id=upper(m[1]);
            json version=nullptr;std::string kind="UNKNOWN",base_id=id;
            if(std::regex_search(name,m,std::regex(R"(\[v([0-9]+)\])",std::regex::icase))) {
                try {version=std::stoull(m[1]);}catch(...){}
            }
            if(!id.empty()) {
                auto number=std::stoull(id,nullptr,16);kind=(number&0xfff)==0x800?"UPDATE":(number&0x1fff)?"DLC":"BASE";
                std::ostringstream formatted;formatted<<std::uppercase<<std::hex<<std::setw(16)<<std::setfill('0')<<(number&~uint64_t(0x1fff));base_id=formatted.str();
            }
            auto clean=std::regex_replace(name,std::regex(R"(\s*\[[^\]]*\])"),"");
            clean=std::regex_replace(clean,std::regex(R"(\.(nsp|nsz|xci)$)",std::regex::icase),"");
            while(!clean.empty()&&std::isspace(static_cast<unsigned char>(clean.back()))) clean.pop_back();
            result["items"].push_back({{"name",clean.empty()?name:clean},{"filename",name},{"titleId",base_id},{"appId",id},{"kind",kind},
                {"version",version},{"sourceUrl",resolve_url(base,url)},{"size",file.is_object()?file.value("size",json(nullptr)):json(nullptr)},
                {"identitySource","filename hint; package validation pending"}});
        }
    }
    {
        std::lock_guard<std::mutex> g(metadata_lock);
        for(auto& item:result["items"]) {
            auto found=metadata.find(text(item,"titleId"));
            if(found!=metadata.end()) {
                item["metadata"]=found->second;
                item["metadata"]["sourceIconUrl"]=text(found->second,"iconUrl");
                item["metadata"]["iconUrl"]="/api/artwork/"+text(item,"titleId");
                auto name=text(found->second,"name"); if(!name.empty()) item["name"]=name;
            }
        }
    }
    {std::lock_guard<std::mutex> guard(library_lock);
        for(const auto& item:result["items"]) if(!text(item,"titleId").empty()&&item["kind"]=="BASE") known_titles[text(item,"titleId")]=text(item,"name");
        const auto path=data_dir+"/library-titles.json";std::ofstream f(path+".tmp");f<<known_titles.dump();f.close();
        if(f) rename((path+".tmp").c_str(),path.c_str());
    }
    std::map<std::string,json> sources;
    for(auto& item:result["items"]) {
        auto source=text(item,"sourceUrl");item.erase("sourceUrl");
        if(!source.empty()) {auto id=entry_id(source);item["entryId"]=id;sources[id]={{"url",source},{"name",item["name"]},{"size",item["size"]},{"titleId",text(item,"titleId")}};}
    }
    {std::lock_guard<std::mutex> guard(sources_lock);download_sources[text(store,"id")]=std::move(sources);}
    return result;
}

int main(int argc,char** argv) {
    std::thread initialization;
    try {
        set_payload_name();
        notify(std::string("PSFoil ")+VERSION+" starting");
        umask(0077);
        int port=argc>1?std::stoi(argv[1]):8466;
        if(port<1024||port>65535) throw std::runtime_error("Invalid listening port");
        httplib::Server server;
        server.set_socket_options([](auto socket){int reuse=1;setsockopt(socket,SOL_SOCKET,SO_REUSEADDR,&reuse,sizeof(reuse));});
        // Claim the port before moving/loading data belonging to an older payload.
        if(!server.bind_to_port("0.0.0.0",port)) throw std::runtime_error("Port already in use or cannot listen");
#ifdef PSFOIL_PS5
        data_dir="/data/psfoil";
        migrate_data("/data/fivefoil"); // Compatibility migration only.
#else
        data_dir=getenv("PSFOIL_DATA_DIR")?getenv("PSFOIL_DATA_DIR"):".psfoil-dev";
        if(!getenv("PSFOIL_DATA_DIR")) migrate_data(".fivefoil-dev");
#endif
        if(mkdir(data_dir.c_str(),0700)!=0 && errno!=EEXIST) throw std::runtime_error("Cannot create PSFoil data directory");
        settings={{"region","US"},{"language","en"},{"theme","red"},{"stores",json::array()}};
        load_settings();
        try {auto saved=read_json(data_dir+"/library-titles.json");if(saved.is_object()) known_titles=std::move(saved);} catch(...) {}
        if(!settings.contains("theme")) settings["theme"]="red";
        if(!settings.contains("appearance")) settings["appearance"]=text(settings,"theme")=="light"?"light":"dark";
        if(text(settings,"theme")=="light") settings["theme"]="red";
        const std::string suffix=":"+std::to_string(port);
        hosts={"localhost"+suffix,"127.0.0.1"+suffix,"[::1]"+suffix};
        ifaddrs* addresses=nullptr;
        if(getifaddrs(&addresses)==0) {
            for(auto* a=addresses;a;a=a->ifa_next) if(a->ifa_addr && a->ifa_addr->sa_family==AF_INET) {
                char ip[INET_ADDRSTRLEN]; auto* sa=reinterpret_cast<sockaddr_in*>(a->ifa_addr);
                if(inet_ntop(AF_INET,&sa->sin_addr,ip,sizeof(ip))) {hosts.insert(std::string(ip)+suffix);if(std::string(ip).rfind("127.",0)!=0) lan_urls.push_back("http://"+std::string(ip)+suffix);}
            }
            freeifaddrs(addresses);
        }
        server.new_task_queue=[] {return new httplib::ThreadPool(4);};
        server.set_payload_max_length(UPLOAD_CHUNK_SIZE);server.set_read_timeout(10);server.set_write_timeout(15);
        server.set_default_headers({{"Cache-Control","no-store"},{"X-Content-Type-Options","nosniff"},
            {"Referrer-Policy","no-referrer"},{"Content-Security-Policy","default-src 'self'; script-src 'self'; style-src 'self'; img-src 'self' https:; object-src 'none'; frame-ancestors 'none'; base-uri 'none'"}});
        server.set_pre_routing_handler([](const auto& req,auto& res) {
            if(!(req.method=="PUT"&&req.path.rfind("/api/uploads/",0)==0)&&req.get_header_value("Content-Length").size()>0) {
                try {if(std::stoull(req.get_header_value("Content-Length"))>64*1024) {reply(res,{{"error","Request too large"}},413);return httplib::Server::HandlerResponse::Handled;}} catch(...) {reply(res,{{"error","Invalid request length"}},400);return httplib::Server::HandlerResponse::Handled;}
            }
            if(!trusted_request(req)) {reply(res,{{"error","Unrecognized host or browser origin"}},403);return httplib::Server::HandlerResponse::Handled;}
            return httplib::Server::HandlerResponse::Unhandled;
        });
        server.set_exception_handler([](const auto&,auto& res,std::exception_ptr e) {
            res.set_header("Connection","close");
            try {if(e) std::rethrow_exception(e);} catch(const json::exception&) {reply(res,{{"error","Invalid request or unexpected catalogue structure"}},400);}
            catch(const std::exception& ex) {reply(res,{{"error",ex.what()}},400);}
        });
        server.Get("/",[](const auto&,auto& r){
            const auto config=public_settings();auto theme=text(config,"theme","red"),appearance=text(config,"appearance","dark");
            if(theme!="red"&&theme!="green"&&theme!="blue"&&theme!="orange"&&theme!="pink"&&theme!="magenta"&&theme!="cyan"&&theme!="purple") theme="red";
            if(appearance!="light") appearance="dark";
            std::string html(reinterpret_cast<const char*>(index_html),index_html_size),bootstrap=config.dump();
            size_t pos=0;while((pos=bootstrap.find('<',pos))!=std::string::npos) {bootstrap.replace(pos,1,"\\u003c");pos+=6;}
            const auto tag=html.find("<html lang=\"en\">");if(tag!=std::string::npos) html.replace(tag,16,"<html lang=\"en\" data-theme=\""+theme+"\" data-appearance=\""+appearance+"\">");
            const auto head=html.find("</head>");html.insert(head,"<script id=\"bootstrapSettings\" type=\"application/json\">"+bootstrap+"</script>");
            r.set_header("Cache-Control","no-store");r.set_content(std::move(html),"text/html; charset=utf-8");
        });
        server.Get("/app.js",[](const auto&,auto& r){r.set_content(reinterpret_cast<const char*>(app_js),app_js_size,"application/javascript");});
        server.Get("/icons.js",[](const auto&,auto& r){r.set_content(reinterpret_cast<const char*>(icons_js),icons_js_size,"application/javascript");});
        server.Get(R"(/icons/([a-z]+))",[](const auto& req,auto& res){for(const auto& icon:ui_icons) {if(icon.name&&req.matches[1].str()==icon.name) {res.set_content(reinterpret_cast<const char*>(icon.data),icon.size,icon.mime);return;}}res.status=404;});
        server.Get("/boot.css",[](const auto&,auto& r){r.set_content(reinterpret_cast<const char*>(boot_css),boot_css_size,"text/css");});
        server.Get("/style.css",[](const auto&,auto& r){r.set_content(reinterpret_cast<const char*>(style_css),style_css_size,"text/css");});
        uploads_routes(server,paths);
        server.Get("/uploads.js",[](const auto&,auto& r){r.set_content(reinterpret_cast<const char*>(uploads_js),uploads_js_size,"application/javascript");});
        server.Get("/api/status",[](const auto&,auto& r){std::lock_guard<std::mutex> guard(tile_lock);auto locations=paths();if(locations.contains("downloadRoot")) locations["staging"]=text(locations,"work");reply(r,{{"version",VERSION},{"lanUrls",lan_urls},{"stage","download manager"},{"downloadsAvailable",true},{"settingsPath",data_dir+"/settings.json"},{"artworkCache",data_dir+"/artwork"},{"paths",locations},{"tile",tile_status},{"metadata",metadata_status()}});});
        server.Get("/api/library",[](const auto&,auto& res){reply(res,installed_games());});
        server.Post("/api/library/delete",[](const auto& req,auto& res){reply(res,delete_installed(json::parse(req.body)));});
        server.Post("/api/library/convert",[](const auto& req,auto& res){reply(res,convert_installed(json::parse(req.body)),202);});
        server.Post("/api/preferences",[](const auto& req,auto& res){
            const auto value=json::parse(req.body);if(!value.contains("showInstalled")||!value["showInstalled"].is_boolean()) throw std::runtime_error("Invalid installed-games preference");
            std::lock_guard<std::mutex> guard(settings_lock);auto next=settings;next["showInstalled"]=value["showInstalled"];save_settings(next);settings=std::move(next);reply(res,{{"saved",true}});
        });
        server.Get(R"(/api/artwork/([0-9A-Fa-f]{16}))",[](const auto& req,auto& res){
            const auto id=upper(req.matches[1].str());if(cached_artwork(id,res)) return;
            std::string source;{std::lock_guard<std::mutex> guard(metadata_lock);auto found=metadata.find(id);if(found!=metadata.end()) source=text(found->second,"iconUrl");}
            // Local games can use titledb artwork without belonging to a configured store.
            json stores;{std::lock_guard<std::mutex> guard(settings_lock);stores=settings["stores"];}
            {std::lock_guard<std::mutex> guard(artwork_lock);for(const auto& store:stores) {
                const auto store_id=text(store,"id");auto found=store_artwork.find(store_id);
                if(source.empty()&&found!=store_artwork.end()&&found->second.count(id)) {res.set_redirect("/api/stores/"+store_id+"/artwork/"+id);return;}
            }}
            if(source.empty()) {res.status=404;return;}const auto url=parse_url(source);if(url.origin.rfind("https://",0)!=0) {res.status=400;return;}
            httplib::Client client(url.origin);client.set_connection_timeout(8);client.set_read_timeout(20);
#ifdef PSFOIL_PS5
            client.set_ca_cert_path("/system/common/cert/CA_LIST.cer");
#endif
            client.enable_server_certificate_verification(true);std::string bytes;
            auto result=client.Get(url.path,[&](const char* p,size_t n){if(quit_requested||n>5*1024*1024-bytes.size()) return false;bytes.append(p,n);return true;});
            const auto mime=image_mime(bytes);if(!result||result->status!=200||mime.empty()) {
                reply(res,{{"error",!result?"Artwork transfer failed: "+httplib::to_string(result.error()):result->status!=200?"Artwork server returned HTTP "+std::to_string(result->status):"Artwork server returned an unsupported image format"},{"titleId",id}},502);return;
            }
            cache_artwork(id,bytes);res.set_header("Cache-Control","private, max-age=86400");res.set_content(std::move(bytes),mime);
        });
        server.Post("/api/tile/install",[port](const auto&,auto& r){std::lock_guard<std::mutex> work(tile_work_lock);const auto result=install_tile(port,true);std::lock_guard<std::mutex> guard(tile_lock);tile_status=result;reply(r,{{"tile",tile_status}});});
        server.Post("/api/quit",[&server](const auto&,auto& res){quit_requested=true;uploads_shutdown();downloads_request_stop();reply(res,{{"stopping",true}});server.stop();});
        server.Get("/api/downloads",[](const auto&,auto& res){reply(res,{{"jobs",download_list()}});});
        server.Post("/api/downloads/clear",[](const auto& req,auto& res){reply(res,{{"cleared",download_clear(text(json::parse(req.body),"id"))}});});
        server.Post(R"(/api/downloads/([a-zA-Z0-9]+)/cancel)",[](const auto& req,auto& res){if(!download_cancel(req.matches[1])) reply(res,{{"error","Download not found"}},404);else reply(res,{{"cancelled",true}});});
        server.Post("/api/downloads",[](const auto& req,auto& res){
            const json value=json::parse(req.body);const auto store_id=text(value,"storeId");json store;
            const bool grouped=value.contains("entryIds");const json ids=grouped?value["entryIds"]:json::array({text(value,"entryId")});
            if(!ids.is_array()||ids.empty()||ids.size()>50) throw std::runtime_error("Choose between 1 and 50 files for a game download");
            std::vector<json> sources;
            {std::lock_guard<std::mutex> guard(sources_lock);auto s=download_sources.find(store_id);
                if(s==download_sources.end()) throw std::runtime_error("Refresh this store before downloading");
                for(const auto& id:ids) {if(!id.is_string()) throw std::runtime_error("Invalid download entry");auto item=s->second.find(id.get<std::string>());if(item==s->second.end()) throw std::runtime_error("Refresh this store before downloading");sources.push_back(item->second);}
            }
            {std::lock_guard<std::mutex> guard(settings_lock);for(const auto& s:settings["stores"]) if(text(s,"id")==store_id) store=s;}
            if(store.is_null()) throw std::runtime_error("Refresh this store before downloading");
            const auto locations=paths();if(locations.contains("error")) throw std::runtime_error("ProsperoEden configuration is unavailable");
            const auto game_id=upper(text(sources.front(),"titleId")),batch_id=grouped?random_hex(12):"";
            std::vector<DownloadSource> jobs;json result_ids=json::array();
            for(const auto& source:sources) {
                if(grouped&&upper(text(source,"titleId"))!=game_id) throw std::runtime_error("Choose files for one game at a time");
                DownloadSource job;job.id=random_hex(12);job.name=text(source,"name");job.url=text(source,"url");job.auth_origin=parse_url(text(store,"url")).origin;
                job.username=text(store,"username");job.password=text(store,"password");job.root=text(locations,"downloadRoot");job.work=text(locations,"work");job.keys=text(locations,"keys");job.split=locations.value("splitDownloads",true);job.batch_id=batch_id;job.game_id=grouped?game_id:"";
                if(source["size"].is_number_unsigned()) job.expected_size=source["size"].get<uint64_t>();
                else if(source["size"].is_number_integer()&&source["size"].get<int64_t>()>=0) job.expected_size=source["size"].get<uint64_t>();
                result_ids.push_back(job.id);jobs.push_back(std::move(job));
            }
            if(grouped) {
                const auto title=text(value,"title",text(sources.front(),"name"));if(title.empty()||title.size()>512) throw std::runtime_error("Invalid game title");
                std::string base_path;
                if(!game_id.empty()) {const auto library=installed_games();for(const auto& item:library["items"]) if(text(item,"titleId")==game_id&&item["kind"]=="BASE") {base_path=text(item,"path");break;}}
                download_enqueue_batch(jobs,title,base_path);reply(res,{{"batchId",batch_id},{"ids",result_ids}},202);
            } else {download_enqueue(jobs.front());reply(res,{{"id",jobs.front().id}},202);}
        });
        server.Get("/api/settings",[](const auto&,auto& r){reply(r,public_settings());});
        server.Get("/api/settings/raw",[](const auto&,auto& res){
            std::lock_guard<std::mutex> guard(settings_lock);const auto path=data_dir+"/settings.json";const bool exists=access(path.c_str(),F_OK)==0;
            auto value=exists?read_json(path):settings;
            if(value.contains("stores")&&value["stores"].is_array()) for(auto& store:value["stores"]) if(store.contains("password")) store["password"]="[redacted]";
            reply(res,{{"path",path},{"exists",exists},{"config",value}});
        });
        server.Post("/api/settings",[](const auto& req,auto& res) {
            auto value=json::parse(req.body);
            const auto region=text(value,"region"),language=text(value,"language");
            auto theme=text(value,"theme","red");
            const auto appearance=text(value,"appearance",theme=="light"?"light":"dark");if(theme=="light") theme="red";
            const auto directory=text(value,"downloadDirectory");
            if(!directory.empty()&&(directory.size()>900||directory[0]!='/'||directory=="/"||std::regex_search(directory,std::regex(R"((^|/)\.\.(/|$))"))||directory.find_first_of("\r\n")!=std::string::npos)) throw std::runtime_error("Choose an absolute custom download directory");
            if(value.contains("splitDownloads")&&!value["splitDownloads"].is_boolean()) throw std::runtime_error("Invalid subfolder option");
            if(theme!="red"&&theme!="green"&&theme!="blue"&&theme!="orange"&&theme!="magenta"&&theme!="pink"&&theme!="cyan"&&theme!="purple") throw std::runtime_error("Unknown accent colour");
            if(appearance!="light"&&appearance!="dark") throw std::runtime_error("Unknown appearance");
            if(!std::regex_match(region,std::regex("[A-Z]{2}"))||!std::regex_match(language,std::regex("[a-z]{2}"))) throw std::runtime_error("Use a two-letter region and language");
            if(!value.contains("stores")||!value["stores"].is_array()||value["stores"].size()>20) throw std::runtime_error("Settings support up to 20 stores");
            std::lock_guard<std::mutex> g(settings_lock);
            json next={{"region",region},{"language",language},{"theme",theme},{"appearance",appearance},{"downloadDirectory",directory},{"splitDownloads",value.value("splitDownloads",true)},{"showInstalled",settings.value("showInstalled",false)},{"stores",json::array()}};
            std::set<std::string> ids;
            for(auto store:value["stores"]) {
                auto id=text(store,"id"); if(id.empty()) id=random_hex(8);
                if(!std::regex_match(id,std::regex("[a-zA-Z0-9_-]{1,64}"))||!ids.insert(id).second) throw std::runtime_error("Invalid or duplicate store ID");
                auto type=text(store,"type");if(type!="ownfoil"&&type!="tinfoil") throw std::runtime_error("Select Ownfoil API or Tinfoil JSON");
                auto name=text(store,"name"),url=text(store,"url"),username=text(store,"username");
                if(name.empty()||name.size()>100||username.size()>256) throw std::runtime_error("Invalid store name or username");
                const auto start=url.find_first_not_of(" \t\r\n"),end=url.find_last_not_of(" \t\r\n");
                url=start==std::string::npos?"":url.substr(start,end-start+1);
                if(!url.empty()&&url.find("://")==std::string::npos) url="https://"+url;
                parse_url(url);
                std::string password=text(store,"password");
                if(!store.contains("password")) for(const auto& old:settings["stores"]) if(text(old,"id")==id) password=text(old,"password");
                if(password.size()>1024) throw std::runtime_error("Password too long");
                next["stores"].push_back({{"id",id},{"name",name},{"type",type},{"url",url},{"username",username},{"password",password}});
            }
            save_settings(next);
            bool changed=region!=text(settings,"region")||language!=text(settings,"language");
            settings=next;
            if(changed) {std::lock_guard<std::mutex> m(metadata_lock);metadata.clear();metadata_loaded=false;}
            reply(res,{{"saved",true}});
        });
        server.Get(R"(/api/stores/([A-Za-z0-9_-]+)/catalogue)",[](const auto& req,auto& res) {
            json selected;
            {std::lock_guard<std::mutex> g(settings_lock);for(auto& store:settings["stores"]) if(text(store,"id")==req.matches[1].str()) selected=store;}
            if(selected.is_null()) {reply(res,{{"error","Store not found"}},404);return;}
            reply(res,catalogue(selected));
        });
        server.Get(R"(/api/stores/([A-Za-z0-9_-]+)/artwork/([0-9A-Fa-f]{16}))",[](const auto& req,auto& res) {
            json store;
            const auto id=req.matches[1].str(),title_id=req.matches[2].str();
            {std::lock_guard<std::mutex> guard(settings_lock);for(const auto& s:settings["stores"]) if(text(s,"id")==id) store=s;}
            if(store.is_null()) {res.status=404;return;}
            if(cached_artwork(title_id,res)) return;
            std::string path;
            {std::lock_guard<std::mutex> guard(artwork_lock);auto s=store_artwork.find(id);if(s!=store_artwork.end()) {auto a=s->second.find(title_id);if(a!=s->second.end()) path=a->second;}}
            if(path.empty()) {res.status=404;return;}
            const auto origin=parse_url(text(store,"url")).origin;
            auto url=parse_url(path[0]=='/'?origin+path:path);
            if(url.origin!=origin && url.origin.rfind("https://",0)!=0) {res.status=400;return;}
            httplib::Client client(url.origin);
            client.set_connection_timeout(8);client.set_read_timeout(20);
#ifdef PSFOIL_PS5
            client.set_ca_cert_path("/system/common/cert/CA_LIST.cer");
#endif
            client.enable_server_certificate_verification(true);
            if(url.origin==origin) client.set_basic_auth(text(store,"username"),text(store,"password"));
            std::string bytes;
            auto result=client.Get(url.path,[&](const char* p,size_t n){if(quit_requested||n>5*1024*1024-bytes.size()) return false;bytes.append(p,n);return true;});
            if(!result||result->status!=200) {res.status=502;return;}
            std::string mime;
            if(bytes.compare(0,8,"\x89PNG\r\n\x1a\n",8)==0) mime="image/png";
            else if(bytes.size()>=3 && static_cast<unsigned char>(bytes[0])==255 && static_cast<unsigned char>(bytes[1])==216 && static_cast<unsigned char>(bytes[2])==255) mime="image/jpeg";
            else if(bytes.size()>=12 && bytes.compare(0,4,"RIFF")==0 && bytes.compare(8,4,"WEBP")==0) mime="image/webp";
            else {res.status=502;return;}
            cache_artwork(title_id,bytes);
            res.set_header("Cache-Control","private, max-age=3600");res.set_content(std::move(bytes),mime);
        });
        server.Post("/api/metadata/refresh",[](const auto&,auto& res) {
            std::lock_guard<std::mutex> serial(metadata_refresh_lock);
            const auto config=public_settings();auto region=text(config,"region"),language=text(config,"language");
            metadata_loading=true;
            struct Loading {~Loading(){metadata_loading=false;}} loading;
            auto records=fetch_metadata_index(region,language);
            import_metadata(records,region,language);
            std::lock_guard<std::mutex> g(settings_lock);
            if(region!=text(settings,"region")||language!=text(settings,"language")) throw std::runtime_error("Metadata region changed; refresh again");
            auto path=data_dir+"/titledb."+region+"."+language+".json";
            std::ofstream f(path+".tmp");f<<records.dump();f.close();
            if(!f || rename((path+".tmp").c_str(),path.c_str())) throw std::runtime_error("Metadata loaded but disk cache could not be saved");
            reply(res,metadata_status());
        });
        downloads_start();
        tile_status="initializing";
        initialization=std::thread([port]{
            {std::lock_guard<std::mutex> serial(metadata_refresh_lock);
                const auto config=public_settings();const auto region=text(config,"region"),language=text(config,"language");
                try {const auto records=read_json(data_dir+"/titledb."+region+"."+language+".json");import_metadata(records,region,language);} catch(...) {}
                metadata_loading=false;
            }
            if(!quit_requested) {std::lock_guard<std::mutex> work(tile_work_lock);const auto result=install_tile(port);std::lock_guard<std::mutex> guard(tile_lock);tile_status=result;}
        });
        notify("PSFoil ready on port "+std::to_string(port));
        server.listen_after_bind();
        quit_requested=true;if(initialization.joinable()) initialization.join();
        uploads_shutdown();downloads_shutdown();
    } catch(const std::exception& e) {uploads_shutdown();quit_requested=true;if(initialization.joinable()) initialization.join();downloads_shutdown();notify(std::string("PSFoil: ")+e.what());return 1;}
    return 0;
}
