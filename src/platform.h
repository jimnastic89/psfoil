// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <string>
void notify(const std::string& message);
std::string install_tile(int port, bool repair=false);
void set_payload_name();
