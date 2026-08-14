#ifndef PM_TINY_INSPECT_RENDERER_H
#define PM_TINY_INSPECT_RENDERER_H

#include "runtime_snapshot.h"

#include <string>

namespace pm_tiny {
namespace cli {

std::string render_inspect_snapshot(const inspect_snapshot &snapshot);

} // namespace cli
} // namespace pm_tiny

#endif
