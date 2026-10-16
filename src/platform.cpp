// SPDX-License-Identifier: GPL-3.0-or-later
#include "platform.h"
#include "assets.h"
#include "json.hpp"
#include <cstdio>
#include <cstring>
#include <cerrno>
#include <sys/stat.h>
#include <unistd.h>
#ifdef PSFOIL_PS5
#include <ps5/kernel.h>
#include <sys/syscall.h>
extern "C" int sceKernelSendNotificationRequest(int, void*, size_t, int);
extern "C" int sceUserServiceInitialize(void*);
extern "C" int sceNetCtlInit(void);
extern "C" int sceKernelLoadStartModule(const char*, size_t, const void*, unsigned, void*, int*);
#endif

void set_payload_name() {
#ifdef PSFOIL_PS5
    syscall(SYS_thr_set_name,-1,"psfoil.elf");
#endif
}

void notify(const std::string& message) {
    printf("%s\n", message.c_str());
    fflush(stdout);
#ifdef PSFOIL_PS5
    struct { char reserved[45]; char message[3075]; } req{};
    snprintf(req.message, sizeof(req.message), "%s", message.c_str());
    sceKernelSendNotificationRequest(0, &req, sizeof(req), 0);
#endif
}

std::string install_tile(int port, bool repair) {
#ifdef PSFOIL_PS5
    int user_service_priority=256;
    sceUserServiceInitialize(&user_service_priority);
    sceNetCtlInit();
    const std::string marker_path=std::string("/data/psfoil/tile-")+tile_asset_id+".ok";
    const char* marker=marker_path.c_str();
    const char* dir = "/user/app/FFOL00001";
    if (!repair && access(marker, F_OK) == 0 && access("/user/app/FFOL00001/sce_sys/icon0.png", F_OK) == 0)
        return "installed";
    // Refresh the existing owned tile's display name during the project rename.
    if(!repair) {
        try {FILE* f=fopen("/user/app/FFOL00001/sce_sys/param.json","rb");if(f) {fclose(f);repair=true;}} catch(...) {}
    }
    uint32_t module = 0;
    if (kernel_dynlib_handle(-1, "libSceAppInstUtil.sprx", &module) != 0) {
        int loaded = sceKernelLoadStartModule("/system/common/lib/libSceAppInstUtil.sprx",0,nullptr,0,nullptr,nullptr);
        if (loaded <= 0 && kernel_dynlib_handle(-1,"libSceAppInstUtil.sprx",&module)!=0) {
            char detail[160];snprintf(detail,sizeof(detail),"AppInstUtil module load failed: 0x%08x",static_cast<unsigned>(loaded));return detail;
        }
        if(loaded>0) module = static_cast<uint32_t>(loaded);
    }
    auto init = reinterpret_cast<int(*)()>(kernel_dynlib_dlsym(-1,module,"sceAppInstUtilInitialize"));
    auto install = reinterpret_cast<int(*)(const char*,const char*,void*)>(kernel_dynlib_dlsym(-1,module,"sceAppInstUtilAppInstallTitleDir"));
    if(!install) install=reinterpret_cast<int(*)(const char*,const char*,void*)>(kernel_dynlib_resolve(-1,module,"Wudg3Xe3heE"));
    auto install_all=reinterpret_cast<int(*)(void*)>(kernel_dynlib_dlsym(-1,module,"sceAppInstUtilAppInstallAll"));
    auto uninstall=reinterpret_cast<int(*)(const char*)>(kernel_dynlib_dlsym(-1,module,"sceAppInstUtilAppUnInstall"));
    if (!init || (!install && !install_all)) return "Tile service symbols unavailable";
    const int init_result=init(); // An already-initialized service may still install successfully.
    if(repair && uninstall) uninstall("FFOL00001");
    if (mkdir(dir,0755) && errno != EEXIST) return std::string("Cannot create tile directory: ")+strerror(errno);
    if (mkdir("/user/app/FFOL00001/sce_sys",0755) && errno != EEXIST) return std::string("Cannot create tile metadata: ")+strerror(errno);
    nlohmann::json param = {{"applicationCategoryType",0},{"titleId","FFOL00001"},
        {"deeplinkUri","http://127.0.0.1:"+std::to_string(port)+"/"},
        {"localizedParameters",{{"defaultLanguage","en-US"},{"en-US",{{"titleName","PSFoil"}}}}}};
    const auto data = param.dump(2);
    auto write = [](const char* path,const void* bytes,size_t size) {
        FILE* f=fopen(path,"wb"); if(!f) return false;
        bool ok=fwrite(bytes,1,size,f)==size; return fclose(f)==0 && ok;
    };
    if (!write("/user/app/FFOL00001/sce_sys/param.json",data.data(),data.size()) ||
        !write("/user/app/FFOL00001/sce_sys/icon0.png",tile_png,tile_png_size)) return "Cannot write tile assets";
    const int result=install?install("FFOL00001","/user/app/",nullptr):-1;
    const int fallback=result && install_all?install_all(nullptr):result;
    if(fallback) {
        char detail[240];snprintf(detail,sizeof(detail),"Tile installation failed: init=0x%08x titleDir=0x%08x installAll=0x%08x",static_cast<unsigned>(init_result),static_cast<unsigned>(result),static_cast<unsigned>(fallback));return detail;
    }
    write(marker,"1\n",2);
    return "installed";
#else
    (void)port;
    (void)repair;
    return "Host preview: tile installation requires PS5";
#endif
}

