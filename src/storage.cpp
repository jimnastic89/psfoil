// SPDX-License-Identifier: GPL-3.0-or-later
#include "storage.h"
#include <cerrno>
#include <stdexcept>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <fcntl.h>
#include <unistd.h>
#include <vector>
#include <cstdlib>

namespace psfoil {
void ensure_directory(const std::string& path) {
    if(path.size()<2||path[0]!='/'||path.find("/../")!=std::string::npos||path.size()>900) throw std::runtime_error("Choose an absolute download directory");
    for(size_t i=1;i<=path.size();i++) if(i==path.size()||path[i]=='/') {
        const auto part=path.substr(0,i);
        if(mkdir(part.c_str(),0700)!=0&&errno!=EEXIST) throw std::runtime_error("Cannot create download directory");
        struct stat st{};
        if(stat(part.c_str(),&st)!=0||!S_ISDIR(st.st_mode)) throw std::runtime_error("Download path is not a directory");
    }
}
void require_space(const std::string& path,uint64_t wanted) {
    struct statvfs fs{};
    if(statvfs(path.c_str(),&fs)!=0) throw std::runtime_error("Cannot check free space in download directory");
    const uint64_t free=uint64_t(fs.f_bavail)*fs.f_frsize;
    if(wanted>free||free-wanted<1024*1024) throw std::runtime_error("Insufficient space for download/conversion");
}
namespace {
struct OwnedFile {
    int fd=-1;
    std::string path;
    struct stat identity{};
    bool keep=false;
    bool unchanged() const {struct stat current{};return fd>=0&&lstat(path.c_str(),&current)==0&&current.st_dev==identity.st_dev&&current.st_ino==identity.st_ino;}
    ~OwnedFile() {if(!keep&&unchanged()) unlink(path.c_str());if(fd>=0) close(fd);}
};
void reserve(OwnedFile& file,const std::string& path) {
    file.path=path;file.fd=open(path.c_str(),O_CREAT|O_EXCL|O_WRONLY,0600);
    if(file.fd<0) {
        if(errno==EEXIST) throw std::runtime_error("Destination file already exists; nothing overwritten: "+path);
        throw std::runtime_error("Cannot reserve destination: "+path+" (errno "+std::to_string(errno)+")");
    }
    if(fstat(file.fd,&file.identity)!=0) throw std::runtime_error("Cannot inspect destination reservation");
}
}
void publish_file(const std::string& source,const std::string& destination,const std::function<void()>& check) {
    OwnedFile reservation;reserve(reservation,destination);check();
    if(!reservation.unchanged()) throw std::runtime_error("Destination changed during publication");
    if(rename(source.c_str(),destination.c_str())==0) {reservation.keep=true;return;}
    if(errno!=EXDEV) throw std::runtime_error("Cannot publish completed file: "+destination+" (errno "+std::to_string(errno)+")");
    // Only the publication copy temporarily lives on the target filesystem.
    // Release the empty reservation while copying, so scans never see a partial NSP.
    if(!reservation.unchanged()||unlink(destination.c_str())!=0) throw std::runtime_error("Destination changed during publication");
    close(reservation.fd);reservation.fd=-1;
    const auto folder=destination.substr(0,destination.find_last_of('/'));
    std::vector<char> name(folder.begin(),folder.end());const std::string suffix="/.psfoil-publish-XXXXXX";name.insert(name.end(),suffix.begin(),suffix.end());name.push_back(0);
    OwnedFile copy;copy.fd=mkstemp(name.data());if(copy.fd<0) throw std::runtime_error("Cannot create destination publication file");
    copy.path=name.data();if(fstat(copy.fd,&copy.identity)!=0) throw std::runtime_error("Cannot inspect publication file");
    OwnedFile input;input.fd=open(source.c_str(),O_RDONLY);input.keep=true;if(input.fd<0) throw std::runtime_error("Cannot read verified staging file");
    struct stat size{};if(fstat(input.fd,&size)!=0||size.st_size<0) throw std::runtime_error("Cannot measure publication source");
    require_space(folder,static_cast<uint64_t>(size.st_size));std::vector<char> buffer(1024*1024);uint64_t copied=0;
    while(true) {
        check();const auto count=read(input.fd,buffer.data(),buffer.size());if(count<0) throw std::runtime_error("Cannot read verified staging file");if(count==0) break;
        size_t written=0;while(written<static_cast<size_t>(count)) {const auto n=write(copy.fd,buffer.data()+written,static_cast<size_t>(count)-written);if(n<=0) throw std::runtime_error("Cannot write publication file");written+=static_cast<size_t>(n);}copied+=static_cast<uint64_t>(count);
    }
    if(copied!=static_cast<uint64_t>(size.st_size)||fsync(copy.fd)!=0) throw std::runtime_error("Cannot flush complete publication file");
    check();reserve(reservation,destination);
    if(!reservation.unchanged()||!copy.unchanged()||rename(copy.path.c_str(),destination.c_str())!=0) throw std::runtime_error("Cannot publish verified file on destination drive");
    reservation.keep=true;copy.keep=true;unlink(source.c_str());
}
}
