#pragma once

#include "daemon_info.h"

#include <string>

namespace pm_tiny { namespace cli {

std::string render_daemon_info(const daemon_info_snapshot &snapshot, bool json);

} } // namespace pm_tiny::cli
