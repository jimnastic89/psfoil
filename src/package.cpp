// SPDX-License-Identifier: GPL-3.0-or-later
// Container/crypto format references: hactool (ISC), NX_Game_Info, and NSZ (MIT).
#include "package.h"
#include <openssl/evp.h>
#include <zstd.h>
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <map>
#include <memory>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <sys/stat.h>
namespace {
using Bytes=std::vector<unsigned char>;
using File=std::unique_ptr<FILE,int(*)(FILE*)>;
uint64_t le(const unsigned char* p,size_t n) {uint64_t v=0;for(size_t i=0;i<n;i++) v|=uint64_t(p[i])<<(i*8);return v;}
void put(unsigned char* p,uint64_t n,size_t count) {for(size_t i=0;i<count;i++) p[i]=static_cast<unsigned char>(n>>(8*i));}
void need(bool condition,const char* message) {if(!condition) throw std::runtime_error(message);}
uint64_t add(uint64_t a,uint64_t b) {need(b<=UINT64_MAX-a,"Container offset overflow");return a+b;}
Bytes read(FILE* f,uint64_t offset,size_t n) {
    need(offset<=INT64_MAX && fseeko(f,static_cast<off_t>(offset),SEEK_SET)==0,"Cannot seek package");
    Bytes b(n);need(fread(b.data(),1,n,f)==n,"Truncated package");return b;
}
void write(FILE* f,const void* bytes,size_t n) {need(fwrite(bytes,1,n,f)==n,"Cannot write decompressed NSP (check free space)");}
uint64_t file_size(FILE* f) {struct stat s{};need(fstat(fileno(f),&s)==0 && s.st_size>=0,"Cannot measure package");return static_cast<uint64_t>(s.st_size);}
std::string hex(const unsigned char* p,size_t n) {static const char* h="0123456789abcdef";std::string out;for(size_t i=0;i<n;i++){out+=h[p[i]>>4];out+=h[p[i]&15];}return out;}
std::string title_hex(uint64_t n) {std::ostringstream s;s<<std::uppercase<<std::hex<<std::setw(16)<<std::setfill('0')<<n;return s.str();}
bool ends(const std::string& s,const std::string& suffix) {return s.size()>=suffix.size()&&s.compare(s.size()-suffix.size(),suffix.size(),suffix)==0;}
struct Entry {std::string name;uint64_t offset,size;};
std::vector<Entry> entries(FILE* f,uint64_t start,uint64_t length,bool hfs=false) {
    need(length>=16,"Container header too small");auto hdr=read(f,start,16);
    need(memcmp(hdr.data(),hfs?"HFS0":"PFS0",4)==0,"Unsupported package container");
    auto count=le(hdr.data()+4,4),strings=le(hdr.data()+8,4);const uint64_t stride=hfs?64:24;
    need(count>0&&count<=8192&&strings<=4*1024*1024,"Invalid package file table");
    uint64_t header=add(16,add(count*stride,strings));need(header<=length,"Truncated file table");
    auto table=read(f,start+16,static_cast<size_t>(header-16));std::vector<Entry> out;
    for(uint64_t i=0;i<count;i++) {
        auto* p=table.data()+i*stride;auto offset=le(p,8),size=le(p+8,8),name=le(p+16,4);
        need(name<strings&&offset<=length-header&&size<=length-header-offset,"File extends beyond package");
        const auto* begin=reinterpret_cast<const char*>(table.data()+count*stride+name);
        auto* end=static_cast<const char*>(memchr(begin,0,strings-name));need(end,"Unterminated package filename");
        std::string filename(begin,end);need(filename.find('/')==std::string::npos&&filename.find('\\')==std::string::npos,"Invalid embedded filename");
        out.push_back({filename,add(start+header,offset),size});
    } return out;
}
std::map<std::string,Bytes> load_keys(const std::string& path) {
    std::ifstream f(path);need(static_cast<bool>(f),"Cannot read ProsperoEden prod.keys");
    std::map<std::string,Bytes> keys;std::string line;
    std::regex pattern("^\\s*([a-zA-Z0-9_]+)\\s*=\\s*([0-9a-fA-F]+)\\s*(?:#.*)?$");
    while(std::getline(f,line)) {std::smatch m;if(!std::regex_match(line,m,pattern)) continue;
        std::string name=m[1],value=m[2];if(value.size()%2||value.size()>128) continue;Bytes b;
        for(size_t i=0;i<value.size();i+=2) b.push_back(static_cast<unsigned char>(std::stoul(value.substr(i,2),nullptr,16)));
        keys[name]=std::move(b);
    }return keys;
}
Bytes key(const std::map<std::string,Bytes>& keys,const std::string& name,size_t size) {
    auto found=keys.find(name);if(found==keys.end()||found->second.size()!=size) throw std::runtime_error("prod.keys is missing required key: "+name);return found->second;
}
Bytes crypt(const Bytes& in,const Bytes& k,const EVP_CIPHER* cipher,const unsigned char* iv,bool encrypt=false) {
    auto ctx=std::unique_ptr<EVP_CIPHER_CTX,decltype(&EVP_CIPHER_CTX_free)>(EVP_CIPHER_CTX_new(),EVP_CIPHER_CTX_free);
    need(ctx&&EVP_CipherInit_ex(ctx.get(),cipher,nullptr,k.data(),iv,encrypt?1:0)==1,"Cannot initialize AES");
    EVP_CIPHER_CTX_set_padding(ctx.get(),0);Bytes out(in.size()+32);int n=0,last=0;
    need(in.size()<=INT32_MAX&&EVP_CipherUpdate(ctx.get(),out.data(),&n,in.data(),static_cast<int>(in.size()))==1&&EVP_CipherFinal_ex(ctx.get(),out.data()+n,&last)==1,"AES operation failed");
    out.resize(n+last);return out;
}
Bytes ctr(const Bytes& input,const Bytes& k,const unsigned char* prefix,uint64_t offset) {
    unsigned char counter[16]{};memcpy(counter,prefix,8);uint64_t block=offset/16;
    for(int i=15;i>=8;i--){counter[i]=static_cast<unsigned char>(block);block>>=8;}
    const size_t skip=offset%16;Bytes padded(skip,0);padded.insert(padded.end(),input.begin(),input.end());
    auto result=crypt(padded,k,EVP_aes_128_ctr(),counter,true);result.erase(result.begin(),result.begin()+skip);return result;
}
Bytes area_key(const std::map<std::string,Bytes>& keys,unsigned generation,unsigned index) {
    need(index<3&&generation<64,"Unsupported NCA key generation");
    const std::string name=std::string("key_area_key_")+std::array<const char*,3>{"application","ocean","system"}[index];
    char suffix[8];snprintf(suffix,sizeof(suffix),"_%02x",generation);
    auto found=keys.find(name+suffix);if(found!=keys.end()&&found->second.size()==16) return found->second;
    auto kek=crypt(key(keys,"aes_kek_generation_source",16),key(keys,std::string("master_key")+suffix,16),EVP_aes_128_ecb(),nullptr);
    auto source=crypt(key(keys,name+"_source",16),kek,EVP_aes_128_ecb(),nullptr);
    return crypt(key(keys,"aes_key_generation_source",16),source,EVP_aes_128_ecb(),nullptr);
}
std::vector<ContentInfo> inspect_nca(FILE* f,const Entry& e,const std::map<std::string,Bytes>& keys) {
    need(e.size>=0xc00,"Truncated metadata NCA");auto header=read(f,e.offset,0xc00);
    bool plain=memcmp(header.data()+0x200,"NCA3",4)==0||memcmp(header.data()+0x200,"NCA2",4)==0;
    if(!plain) {
        auto k=key(keys,"header_key",32);const auto encrypted=header;
        for(size_t sector=0;sector<6;sector++) {unsigned char tweak[16]{};
            const bool nca2=sector>=2&&memcmp(header.data()+0x200,"NCA2",4)==0;
            tweak[15]=nca2?0:static_cast<unsigned char>(sector);
            Bytes part(encrypted.begin()+sector*512,encrypted.begin()+(sector+1)*512);auto dec=crypt(part,k,EVP_aes_128_xts(),tweak);
            std::copy(dec.begin(),dec.end(),header.begin()+sector*512);
        }
        need(memcmp(header.data()+0x200,"NCA3",4)==0||memcmp(header.data()+0x200,"NCA2",4)==0,"Unable to decrypt NCA header; check prod.keys");
    }
    need(le(header.data()+0x208,8)==e.size,"NCA length does not match its header");
    if(header[0x205]!=1) return {};
    std::vector<ContentInfo> out;
    for(int i=0;i<4;i++) {
        const auto section=le(header.data()+0x240+i*16,4)*512,end=le(header.data()+0x244+i*16,4)*512;
        if(section==0) continue;
        need(end>=section&&end<=e.size,"Invalid metadata section bounds");
        auto* fs=header.data()+0x400+i*512;if(fs[2]!=1||fs[3]!=2) continue;
        auto pfs_offset=le(fs+0x40,8),pfs_size=le(fs+0x48,8);
        need(pfs_size<=32*1024*1024&&pfs_offset<=end-section&&pfs_size<=end-section-pfs_offset,"Invalid metadata filesystem");
        auto data=read(f,e.offset+section+pfs_offset,static_cast<size_t>(pfs_size));
        if(!plain&&fs[4]!=1) {
            need(fs[4]==3,"Unsupported metadata encryption");
            need(std::all_of(header.begin()+0x230,header.begin()+0x240,[](unsigned char c){return c==0;}),"Rights-encrypted metadata is not supported");
            unsigned generation=std::max(header[0x206],header[0x220]);if(generation) --generation;
            auto ak=area_key(keys,generation,header[0x207]);Bytes encrypted(header.begin()+0x320,header.begin()+0x330);
            auto content_key=crypt(encrypted,ak,EVP_aes_128_ecb(),nullptr);unsigned char prefix[8];
            for(int j=0;j<8;j++) prefix[j]=fs[0x147-j];
            data=ctr(data,content_key,prefix,section+pfs_offset);
        }
        need(data.size()>=16&&memcmp(data.data(),"PFS0",4)==0,"Cannot read metadata filesystem; check prod.keys");
        auto count=le(data.data()+4,4),strings=le(data.data()+8,4);auto base=add(16,add(count*24,strings));
        need(count<=8192&&base<=data.size(),"Invalid CNMT file table");
        for(uint64_t j=0;j<count;j++) {
            auto* p=data.data()+16+j*24;auto offset=le(p,8),size=le(p+8,8),name=le(p+16,4);
            need(name<strings&&offset<=data.size()-base&&size<=data.size()-base-offset,"Invalid CNMT entry");
            auto* s=reinterpret_cast<char*>(data.data()+16+count*24+name);auto* nul=static_cast<char*>(memchr(s,0,strings-name));need(nul,"Invalid CNMT filename");
            if(!ends(std::string(s,nul),".cnmt")) continue;
            need(size>=32,"Truncated CNMT");auto* cnmt=data.data()+base+offset;
            need(32+le(cnmt+14,2)+le(cnmt+16,2)*56+le(cnmt+18,2)*16<=size,"Truncated CNMT content records");
            auto id=le(cnmt,8);auto type=cnmt[12];std::string kind=type==0x80?"BASE":type==0x81?"UPDATE":type==0x82?"DLC":"";
            need(!kind.empty(),"Unsupported content metadata type");
            uint64_t parent=id;
            if(type!=0x80) {need(size>=40,"Missing CNMT parent title");parent=le(cnmt+32,8);}
            out.push_back({title_hex(id),title_hex(parent),kind,static_cast<uint32_t>(le(cnmt+8,4))});
        }
    }
    need(!out.empty(),"No application CNMT found");return out;
}
struct Section {uint64_t offset,size,crypto;Bytes key,counter;};
uint64_t decompress_ncz(FILE* input,const Entry& entry,FILE* output,const PackageProgress& progress,uint64_t overall,uint64_t total) {
    need(entry.size>=0x4010,"Truncated NCZ");auto header=read(input,entry.offset,0x4000),magic=read(input,entry.offset+0x4000,16);
    need(memcmp(magic.data(),"NCZSECTN",8)==0,"Invalid NCZ section header");auto count=le(magic.data()+8,8);
    need(count>0&&count<=64&&0x4010+count*64<=entry.size,"Invalid NCZ sections");
    auto raw=read(input,entry.offset+0x4010,static_cast<size_t>(count*64));std::vector<Section> sections;
    uint64_t expected=0x4000;
    for(uint64_t i=0;i<count;i++) {
        auto* p=raw.data()+i*64;auto start=le(p,8),size=le(p+8,8),crypto=le(p+16,8);
        if(i==0&&start>0x4000) sections.push_back({0x4000,start-0x4000,1,{},{}});
        if(i==0&&start<0x4000) {need(size>=0x4000-start,"Invalid first NCZ section");size-=0x4000-start;start=0x4000;}
        if(!sections.empty()) need(start==add(sections.back().offset,sections.back().size),"Non-contiguous NCZ sections");
        else need(start==0x4000,"Invalid NCZ start");
        need(size>0&&(crypto==1||crypto==3||crypto==4),"Unsupported NCZ crypto type");
        expected=add(start,size);sections.push_back({start,size,crypto,Bytes(p+32,p+48),Bytes(p+48,p+64)});
    }
    auto digest=std::unique_ptr<EVP_MD_CTX,decltype(&EVP_MD_CTX_free)>(EVP_MD_CTX_new(),EVP_MD_CTX_free);
    need(digest&&EVP_DigestInit_ex(digest.get(),EVP_sha256(),nullptr)==1,"Cannot hash NCA");
    write(output,header.data(),header.size());EVP_DigestUpdate(digest.get(),header.data(),header.size());
    size_t section_id=0;uint64_t position=0x4000;
    auto emit=[&](const unsigned char* p,size_t n) {
        while(n) {
            need(section_id<sections.size(),"NCZ expands beyond its sections");auto& section=sections[section_id];
            auto amount=static_cast<size_t>(std::min<uint64_t>(n,section.offset+section.size-position));
            need(amount>0,"Invalid NCZ section progress");Bytes bytes(p,p+amount);
            if(section.crypto==3||section.crypto==4) bytes=ctr(bytes,section.key,section.counter.data(),position);
            write(output,bytes.data(),bytes.size());EVP_DigestUpdate(digest.get(),bytes.data(),bytes.size());
            position+=amount;p+=amount;n-=amount;if(position==section.offset+section.size) ++section_id;
        }
        progress(overall+position,total);
    };
    uint64_t cursor=entry.offset+0x4010+count*64,end=entry.offset+entry.size;
    auto block_magic=read(input,cursor,8);
    if(memcmp(block_magic.data(),"NCZBLOCK",8)==0) {
        auto block=read(input,cursor,24);cursor+=24;unsigned exponent=block[11];auto blocks=le(block.data()+12,4),uncompressed=le(block.data()+16,8);
        need(block[8]<=2&&exponent>=14&&exponent<=27,"Unsupported NCZ block format/size (maximum 128 MiB)");
        const uint64_t block_size=uint64_t(1)<<exponent;
        need(uncompressed==expected-0x4000&&blocks==(uncompressed+block_size-1)/block_size&&blocks<=4000000,"Invalid NCZ block table");
        auto sizes=read(input,cursor,static_cast<size_t>(blocks*4));cursor+=blocks*4;
        for(uint64_t i=0;i<blocks;i++) {
            auto compressed=le(sizes.data()+i*4,4),wanted=std::min(block_size,uncompressed-i*block_size);
            need(compressed>0&&compressed<=wanted&&cursor<=end&&compressed<=end-cursor,"Invalid NCZ block bounds");
            auto bytes=read(input,cursor,static_cast<size_t>(compressed));cursor+=compressed;
            if(compressed<wanted) {Bytes decoded(static_cast<size_t>(wanted));auto r=ZSTD_decompress(decoded.data(),decoded.size(),bytes.data(),bytes.size());need(!ZSTD_isError(r)&&r==wanted,"NCZ block decompression failed");emit(decoded.data(),decoded.size());}
            else emit(bytes.data(),bytes.size());
        }
        need(cursor==end,"Trailing data in NCZ block stream");
    } else {
        auto stream=std::unique_ptr<ZSTD_DStream,decltype(&ZSTD_freeDStream)>(ZSTD_createDStream(),ZSTD_freeDStream);
        need(stream&&!ZSTD_isError(ZSTD_initDStream(stream.get())),"Cannot initialize Zstandard");
        need(!ZSTD_isError(ZSTD_DCtx_setParameter(stream.get(),ZSTD_d_windowLogMax,27)),"Cannot limit Zstandard window");
        Bytes decoded(1024*1024);size_t remaining=1;
        while(cursor<end) {
            auto chunk=read(input,cursor,static_cast<size_t>(std::min<uint64_t>(1024*1024,end-cursor)));cursor+=chunk.size();ZSTD_inBuffer in{chunk.data(),chunk.size(),0};
            while(true) {
                ZSTD_outBuffer out{decoded.data(),decoded.size(),0};auto before=in.pos;
                remaining=ZSTD_decompressStream(stream.get(),&out,&in);need(!ZSTD_isError(remaining),"Invalid/truncated Zstandard stream");
                if(out.pos) emit(decoded.data(),out.pos);
                if(in.pos==in.size && (out.pos<out.size||remaining==0)) break;
                need(out.pos||in.pos>before,"Zstandard made no progress");
            }
        }
        need(remaining==0,"Incomplete Zstandard frame");
    }
    need(position==expected,"NCZ output length mismatch");
    unsigned char hash[EVP_MAX_MD_SIZE];unsigned n=0;EVP_DigestFinal_ex(digest.get(),hash,&n);
    const auto name=entry.name.substr(0,entry.name.size()-4);
    need(std::regex_match(name,std::regex("[0-9a-fA-F]{32}")),"NCZ filename does not contain its NCA hash");
    std::string lower=name;std::transform(lower.begin(),lower.end(),lower.begin(),[](unsigned char c){return std::tolower(c);});
    need(hex(hash,16)==lower,"Reconstructed NCA hash mismatch");return position;
}
} // namespace

bool package_is_nsz(const std::string& path) {
    File f(fopen(path.c_str(),"rb"),fclose);need(static_cast<bool>(f),"Cannot open package");
    auto prefix=read(f.get(),0,4);if(memcmp(prefix.data(),"PFS0",4)!=0) return false;
    auto files=entries(f.get(),0,file_size(f.get()));return std::any_of(files.begin(),files.end(),[](const Entry& e){return ends(e.name,".ncz");});
}

std::vector<ContentInfo> inspect_package(const std::string& path,const std::string& keys_path,bool compressed_metadata) {
    File f(fopen(path.c_str(),"rb"),fclose);need(static_cast<bool>(f),"Cannot open downloaded package");auto length=file_size(f.get());
    auto prefix=read(f.get(),0,4);std::vector<Entry> files;
    if(memcmp(prefix.data(),"PFS0",4)==0) files=entries(f.get(),0,length);
    else {
        auto header=read(f.get(),0,0x140);need(memcmp(header.data()+0x100,"HEAD",4)==0,"File is neither NSP nor XCI");
        auto offset=le(header.data()+0x130,8);need(offset<length,"Invalid XCI root offset");auto root=entries(f.get(),offset,length-offset,true);
        auto secure=std::find_if(root.begin(),root.end(),[](const Entry& e){return e.name=="secure";});need(secure!=root.end(),"XCI has no secure partition");
        files=entries(f.get(),secure->offset,secure->size,true);
    }
    auto keys=load_keys(keys_path);std::vector<ContentInfo> content;
    need(compressed_metadata||!std::any_of(files.begin(),files.end(),[](const Entry& e){return ends(e.name,".ncz");}),"Compressed entries remain in package; XCZ is unsupported");
    for(const auto& e:files) if(ends(e.name,".nca")) {
        auto found=inspect_nca(f.get(),e,keys);content.insert(content.end(),found.begin(),found.end());
    }
    need(!content.empty(),"Package contains no readable application content metadata");return content;
}
void decompress_nsz(const std::string& input_path,const std::string& output_path,const PackageProgress& progress) {
    File input(fopen(input_path.c_str(),"rb"),fclose);need(static_cast<bool>(input),"Cannot read NSZ");auto length=file_size(input.get());auto files=entries(input.get(),0,length);
    auto hdr=read(input.get(),0,16);auto strings=le(hdr.data()+8,4);uint64_t header=16+files.size()*24+strings;
    File output(fopen(output_path.c_str(),"wb+"),fclose);need(static_cast<bool>(output),"Cannot create NSP output");
    need(fseeko(output.get(),static_cast<off_t>(header),SEEK_SET)==0,"Cannot reserve NSP header");
    std::vector<uint64_t> sizes;uint64_t offset=header;
    for(auto& file:files) {
        uint64_t written=0;
        if(ends(file.name,".ncz")) {written=decompress_ncz(input.get(),file,output.get(),progress,offset,length);file.name.back()='a';}
        else {
            while(written<file.size) {auto bytes=read(input.get(),file.offset+written,static_cast<size_t>(std::min<uint64_t>(1024*1024,file.size-written)));write(output.get(),bytes.data(),bytes.size());written+=bytes.size();progress(offset+written,length);}
        }
        sizes.push_back(written);offset=add(offset,written);
    }
    Bytes table(static_cast<size_t>(header),0);memcpy(table.data(),"PFS0",4);put(table.data()+4,files.size(),4);put(table.data()+8,strings,4);
    uint64_t position=0,name_offset=0;
    for(size_t i=0;i<files.size();i++) {auto* p=table.data()+16+i*24;put(p,position,8);put(p+8,sizes[i],8);put(p+16,name_offset,4);
        need(name_offset+files[i].name.size()+1<=strings,"NSP string table overflow");memcpy(table.data()+16+files.size()*24+name_offset,files[i].name.c_str(),files[i].name.size()+1);name_offset+=files[i].name.size()+1;position+=sizes[i];}
    need(fseeko(output.get(),0,SEEK_SET)==0,"Cannot finalize NSP header");write(output.get(),table.data(),table.size());need(fflush(output.get())==0,"Cannot flush NSP output");
}
