// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "json.hpp"
#include <functional>
namespace httplib {class Server;}

inline constexpr size_t UPLOAD_CHUNK_SIZE=4*1024*1024;
void uploads_routes(httplib::Server& server,std::function<nlohmann::json()> locations);
void uploads_shutdown();
