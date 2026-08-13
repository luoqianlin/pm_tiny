#include "core/memory_util.h"

#include <cstdlib>
#include <cstring>
#include <iostream>

namespace {

void expect(const std::string &actual, const std::string &expected, const char *message) {
    if (actual != expected) {
        std::cerr << message << " expected=`" << expected << "` actual=`" << actual << "`\n";
        std::abort();
    }
}

} // namespace

int main() {
    using pm_tiny::utils::memory::to_human_readable_size;
    expect(to_human_readable_size(512), "512.00KB", "should render kilobytes");
    expect(to_human_readable_size(4096), " 4.00MB", "should render megabytes");
    expect(to_human_readable_size(5LL * 1024 * 1024), " 5.00GB", "should render gigabytes");
    return 0;
}
