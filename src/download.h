// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "json.hpp"
#include <string>
#include <vector>
struct DownloadSource {
    std::string id,name,url,auth_origin,username,password,root,work,keys,batch_id,game_id;
    // Local conversions preserve their source until verification; uploads own staging files.
    std::string local_path,local_canonical,batch_title,batch_base,uploaded_path;
    uint64_t local_device=0,local_inode=0,local_size=0;
    int64_t local_modified=0;
    uint64_t expected_size=0;
    bool split=true;
};
void downloads_start();
void downloads_request_stop();
void downloads_shutdown();
void download_enqueue(const DownloadSource& source);
void download_enqueue_batch(const std::vector<DownloadSource>& sources,const std::string& title,const std::string& base_path="");
size_t download_clear(const std::string& id="");
nlohmann::json download_list();
bool download_cancel(const std::string& id);
