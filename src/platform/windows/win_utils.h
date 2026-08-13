#pragma once

#include <string>
#include <vector>

namespace pm_tiny {
namespace win {

std::string trim_copy(const std::string &text);
void trim_inplace(std::string &text);
std::string strip_quotes(const std::string &text);
bool iequals(const std::string &lhs, const std::string &rhs);
bool parse_bool(const std::string &text, bool default_value);
int parse_int(const std::string &text, int default_value);
std::vector<std::string> split_and_trim(const std::string &text, char delimiter);
std::vector<std::string> parse_list_value(const std::string &text);

std::vector<wchar_t> build_environment_block(const std::vector<std::string> &overrides_utf8);

std::string control_pipe_name();
std::wstring control_pipe_name_wide();

std::wstring utf8_to_wide(const std::string &text);
std::string wide_to_utf8(const std::wstring &text);

} // namespace win
} // namespace pm_tiny
