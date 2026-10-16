// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
#include <string>
#include <functional>

namespace psfoil {
void ensure_directory(const std::string& path);
void require_space(const std::string& path,uint64_t wanted);
void publish_file(const std::string& source,const std::string& destination,const std::function<void()>& check);
}
