#pragma once

#include <string>
#include <vector>

#include "prog_cfg.h"

#include <yaml-cpp/yaml.h>

namespace YAML {
class Node;
}

namespace pm_tiny {
struct ProgCfgParseResult {
    bool success = false;
    std::string error;
    std::vector<std::string> warnings;
};

struct ProgCfgDocumentParseResult {
    bool success = false;
    std::string error;
    std::vector<std::string> warnings;
    std::vector<prog_cfg_t> programs;
};

struct ProgCfgSerializeOptions {
    bool include_run_as = true;
    bool include_oom_score_adj = true;
    bool include_pty = true;
};

ProgCfgParseResult parse_prog_cfg_yaml_node(const YAML::Node &node, prog_cfg_t &out_cfg);
ProgCfgDocumentParseResult parse_prog_cfg_yaml_document(const YAML::Node &root);
YAML::Node serialize_prog_cfg_yaml_node(const prog_cfg_t &cfg,
                                        const ProgCfgSerializeOptions &options = {});

} // namespace pm_tiny
