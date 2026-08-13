#include "prog_cfg.h"
#include "prog_cfg_order.h"

#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {

void expect(bool condition, const char *message) {
    if (!condition) {
        std::cerr << message << std::endl;
        std::abort();
    }
}

pm_tiny::prog_cfg_t config(const std::string &name, std::vector<std::string> dependencies = {}) {
    pm_tiny::prog_cfg_t value;
    value.name = name;
    value.depends_on = std::move(dependencies);
    return value;
}

} // namespace

int main() {
    std::string error;
    std::vector<pm_tiny::prog_cfg_t> valid = {
        config("service", {"database"}), config("database"), config("worker", {"service"})
    };
    expect(pm_tiny::validate_and_order_prog_cfgs(valid, error), "valid dependencies should order");
    expect(valid[0].name == "database" && valid[1].name == "service" && valid[2].name == "worker",
           "topological order should preserve the first available source order");

    auto duplicate = std::vector<pm_tiny::prog_cfg_t>{config("same"), config("same")};
    expect(!pm_tiny::validate_and_order_prog_cfgs(duplicate, error) &&
           error == "Duplicate program name: same", "duplicate names should have a stable error");

    auto missing = std::vector<pm_tiny::prog_cfg_t>{config("app", {"missing"})};
    expect(!pm_tiny::validate_and_order_prog_cfgs(missing, error) &&
           error == "Program `app` depends on missing `missing`", "missing dependency should have a stable error");

    auto cycle = std::vector<pm_tiny::prog_cfg_t>{config("a", {"b"}), config("b", {"a"})};
    expect(!pm_tiny::validate_and_order_prog_cfgs(cycle, error) &&
           error == "Program dependency cycle detected: a -> b -> a", "cycle should include its path");
    return 0;
}
