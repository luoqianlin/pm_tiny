#include "program_name.h"

#include <cctype>

namespace pm_tiny {

bool is_valid_program_name(const std::string &name) {
    if (name.empty() || name.size() > program_name_max_length) return false;
    const auto valid_first = [](unsigned char ch) { return std::isalnum(ch) != 0; };
    const auto valid_rest = [&](unsigned char ch) {
        return valid_first(ch) || ch == '.' || ch == '_' || ch == '-';
    };
    if (!valid_first(static_cast<unsigned char>(name.front()))) return false;
    for (std::size_t index = 1; index < name.size(); ++index) {
        if (!valid_rest(static_cast<unsigned char>(name[index]))) return false;
    }
    return true;
}

std::string program_name_validation_error(const std::string &name) {
    if (name.empty()) return "Program name must not be empty";
    if (name.size() > program_name_max_length)
        return "Program name must be at most 128 characters";
    return "Invalid program name `" + name + "`; expected [A-Za-z0-9][A-Za-z0-9._-]*";
}

} // namespace pm_tiny
