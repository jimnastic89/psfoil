// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>
struct ContentInfo { std::string title_id,base_id,kind; uint32_t version; };
using PackageProgress=std::function<void(uint64_t,uint64_t)>;
bool package_is_nsz(const std::string& path);
std::vector<ContentInfo> inspect_package(const std::string& path,const std::string& keys,bool compressed_metadata=false);
void decompress_nsz(const std::string& input,const std::string& output,const PackageProgress& progress);
