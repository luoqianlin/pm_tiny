#include "process_list_renderer.h"

#include "pm_tiny.h"

#include <fort.hpp>
#include <nlohmann/json.hpp>
#include <utf8proc.h>

#include <algorithm>
#include <cstdlib>
#include <iomanip>
#include <limits>
#include <sstream>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/ioctl.h>
#include <unistd.h>
#endif

namespace pm_tiny {
namespace cli {
namespace {

using nlohmann::json;

struct grapheme_span {
    std::size_t begin = 0;
    std::size_t end = 0;
    std::size_t width = 0;
};

bool split_graphemes(const utf8proc_uint8_t *data, std::size_t length,
                     std::vector<grapheme_span> &spans) {
    spans.clear();
    std::size_t offset = 0;
    utf8proc_int32_t previous = -1;
    utf8proc_int32_t break_state = 0;
    while (offset < length) {
        utf8proc_int32_t codepoint = 0;
        const auto consumed = utf8proc_iterate(data + offset, length - offset, &codepoint);
        if (consumed <= 0) return false;
        const bool starts_cluster = previous < 0 ||
            utf8proc_grapheme_break_stateful(previous, codepoint, &break_state);
        if (starts_cluster) spans.push_back({offset, offset, 0});
        auto &span = spans.back();
        span.end = offset + static_cast<std::size_t>(consumed);
        // A grapheme such as an emoji ZWJ sequence occupies the maximum width
        // of one of its code points, not the sum of all code-point widths.
        span.width = std::max(span.width,
                              static_cast<std::size_t>(std::max(0, utf8proc_charwidth(codepoint))));
        previous = codepoint;
        offset = span.end;
    }
    return true;
}

int utf8_width_range(const void *begin, const void *end, std::size_t *width) {
    const auto *cursor = static_cast<const utf8proc_uint8_t *>(begin);
    const auto *finish = static_cast<const utf8proc_uint8_t *>(end);
    std::vector<grapheme_span> spans;
    if (!split_graphemes(cursor, static_cast<std::size_t>(finish - cursor), spans)) return -1;
    *width = 0;
    for (const auto &span : spans) *width += span.width;
    return 0;
}

std::size_t display_width(const std::string &text) {
    std::size_t width = 0;
    if (utf8_width_range(text.data(), text.data() + text.size(), &width) == 0) return width;
    return text.size();
}

std::string sanitize_table_text(const std::string &input) {
    std::string output;
    output.reserve(input.size());
    const auto *cursor = reinterpret_cast<const utf8proc_uint8_t *>(input.data());
    const auto *finish = cursor + input.size();
    while (cursor < finish) {
        utf8proc_int32_t codepoint = 0;
        const auto consumed = utf8proc_iterate(cursor, finish - cursor, &codepoint);
        if (consumed <= 0) {
            output += "?";
            ++cursor;
            continue;
        }
        const auto category = utf8proc_category(codepoint);
        if (category == UTF8PROC_CATEGORY_CC || category == UTF8PROC_CATEGORY_ZL ||
            category == UTF8PROC_CATEGORY_ZP) {
            output.push_back(' ');
        } else {
            output.append(reinterpret_cast<const char *>(cursor), static_cast<std::size_t>(consumed));
        }
        cursor += consumed;
    }
    return output;
}

std::string truncate_prefix(const std::string &input, std::size_t max_width) {
    if (display_width(input) <= max_width) return input;
    if (max_width == 0) return {};
    const std::string ellipsis = "…";
    if (max_width == 1) return ellipsis;
    const std::size_t target = max_width - 1;
    std::vector<grapheme_span> spans;
    if (!split_graphemes(reinterpret_cast<const utf8proc_uint8_t *>(input.data()), input.size(), spans)) {
        return ellipsis;
    }
    std::size_t width = 0;
    std::size_t end = 0;
    for (const auto &span : spans) {
        if (width + span.width > target) break;
        width += span.width;
        end = span.end;
    }
    return input.substr(0, end) + ellipsis;
}

std::string truncate_suffix(const std::string &input, std::size_t max_width) {
    if (display_width(input) <= max_width) return input;
    if (max_width == 0) return {};
    if (max_width == 1) return "…";
    std::vector<grapheme_span> spans;
    if (!split_graphemes(reinterpret_cast<const utf8proc_uint8_t *>(input.data()), input.size(), spans)) {
        return "…";
    }
    std::size_t width = 0;
    std::size_t start = input.size();
    for (auto iter = spans.rbegin(); iter != spans.rend(); ++iter) {
        if (width + iter->width > max_width - 1) break;
        width += iter->width;
        start = iter->begin;
    }
    return "…" + input.substr(start);
}

std::string format_duration(std::int64_t milliseconds) {
    if (milliseconds < 0) return "-";
    const auto seconds = milliseconds / 1000;
    const auto days = seconds / 86400;
    const auto hours = (seconds % 86400) / 3600;
    const auto minutes = (seconds % 3600) / 60;
    const auto remaining_seconds = seconds % 60;
    std::ostringstream output;
    if (days > 0) output << days << "d" << hours << "h";
    else if (hours > 0) output << hours << "h" << minutes << "m";
    else if (minutes > 0) output << minutes << "m" << remaining_seconds << "s";
    else output << remaining_seconds << "s";
    return output.str();
}

std::string format_memory(std::int64_t kib) {
    if (kib < 0) return "-";
    std::ostringstream output;
    output << std::fixed << std::setprecision(2);
    if (kib >= 1024LL * 1024LL) output << kib / (1024.0 * 1024.0) << "GB";
    else if (kib >= 1024) output << kib / 1024.0 << "MB";
    else output << kib << "KB";
    return output.str();
}

std::string join_dependencies(const std::vector<std::string> &dependencies) {
    std::string result;
    for (const auto &dependency : dependencies) {
        if (!result.empty()) result += ',';
        result += dependency;
    }
    return result;
}

std::string shell_preview(const process_list_entry &entry) {
    auto quote = [](const std::string &value) {
        if (!value.empty() && value.find_first_of(" \t\r\n'\"\\") == std::string::npos) return value;
        std::string out = "'";
        for (const auto ch : value) out += ch == '\'' ? "'\\''" : std::string(1, ch);
        return out + "'";
    };
    std::string result = quote(entry.executable);
    for (const auto &arg : entry.args) result += " " + quote(arg);
    return result;
}

fort::color state_color(std::int32_t state) {
    switch (state) {
        case PM_TINY_PROG_STATE_RUNING: return fort::color::green;
        case PM_TINY_PROG_STATE_STARTING: return fort::color::blue;
        case PM_TINY_PROG_STATE_WAITING_START: return fort::color::yellow;
        case PM_TINY_PROG_STATE_EXIT: return fort::color::blue;
        default: return fort::color::red;
    }
}

struct column_spec {
    std::string title;
    std::size_t width;
    bool right_align = false;
};

std::string json_output(const std::vector<process_list_entry> &entries) {
    json root;
    root["schema_version"] = process_list_schema_version;
    root["total"] = entries.size();
    root["processes"] = json::array();
    for (const auto &entry : entries) {
        json item;
        item["name"] = entry.name;
        item["pid"] = entry.pid >= 0 ? json(entry.pid) : json(nullptr);
        item["state"] = pm_state_to_str(entry.state);
        item["state_code"] = entry.state;
        item["uptime_ms"] = entry.has_uptime ? json(entry.uptime_ms) : json(nullptr);
        item["restart_count"] = entry.restart_count;
        item["memory_kib"] = entry.has_rss ? json(entry.rss_kib) : json(nullptr);
        item["daemon"] = entry.daemon;
        if (entry.pty == pty_mode_t::unsupported) item["pty"] = nullptr;
        else item["pty"] = entry.pty == pty_mode_t::enabled;
        item["depends_on"] = entry.depends_on;
        item["cwd"] = entry.cwd;
        item["executable"] = entry.executable;
        item["args"] = entry.args;
        item["restart_pending"] = entry.restart_pending;
        item["restart_delay_remaining_ms"] = entry.restart_pending ?
            json(entry.restart_delay_remaining_ms) : json(nullptr);
        item["restart_attempts_in_window"] = entry.restart_attempts_in_window;
        item["restart_suppressed"] = entry.restart_suppressed;
        item["restart_suppression_reason"] = entry.restart_suppressed ?
            json(entry.restart_suppression_reason) : json(nullptr);
        item["generation"] = entry.generation;
        item["ready"] = entry.ready;
        item["heartbeat_enabled"] = entry.heartbeat_enabled;
        item["last_exit_reason"] = entry.has_last_exit ? json(entry.last_exit_reason) : json(nullptr);
        item["last_exit_code"] = entry.has_last_exit ? json(entry.last_exit_code) : json(nullptr);
        item["process_tree_backend"] = entry.process_tree_backend.empty() ?
            json(nullptr) : json(entry.process_tree_backend);
        item["process_tree_degraded"] = entry.process_tree_degraded;
        item["process_tree_degradation_reason"] = entry.process_tree_degraded ?
            json(entry.process_tree_degradation_reason) : json(nullptr);
        item["config_source"] = entry.config_source.empty() ? json(nullptr) : json(entry.config_source);
        item["log_degraded"] = entry.log_degraded;
        item["log_dropped_bytes"] = entry.log_dropped_bytes;
        item["log_last_error"] = entry.log_last_error.empty() ? json(nullptr) : json(entry.log_last_error);
        item["log_retry_remaining_ms"] = entry.log_degraded ? json(entry.log_retry_remaining_ms) : json(nullptr);
        item["log_paths"] = entry.log_paths;
        root["processes"].push_back(std::move(item));
    }
    return root.dump(2) + "\n";
}

} // namespace

std::string render_process_list(const std::vector<process_list_entry> &entries,
                                const list_render_options &options) {
    if (options.json) return json_output(entries);
    if (entries.empty()) return "Total: 0\n";

    std::size_t terminal_width = options.terminal_width == 0 ? 120 : options.terminal_width;
    std::vector<column_spec> columns;
    if (options.wide) {
        columns = {{"name", 16}, {"pid", 8, true}, {"state", 8}, {"uptime", 8, true},
                   {"restarts", 8, true}, {"retry", 10}, {"log", 8}, {"memory", 10, true},
                   {"daemon", 6}, {"pty", 3}, {"depends_on", 14}, {"cwd", 22}, {"command", 28}};
        const std::size_t fixed_overhead = columns.size() * 3 + 1;
        const std::size_t minimum_content = 61;
        if (terminal_width > fixed_overhead + minimum_content) {
            std::size_t extra = terminal_width - fixed_overhead - minimum_content;
            columns[12].width = 7 + extra * 48 / 100;
            columns[11].width = 3 + extra * 27 / 100;
            columns[0].width = 4 + extra * 15 / 100;
            columns[10].width = 10 + extra * 10 / 100;
        } else {
            columns[0].width = 4;
            columns[10].width = 10;
            columns[11].width = 3;
            columns[12].width = 7;
        }
    } else {
        columns = {{"name", 24}, {"pid", 8, true}, {"state", 8}, {"uptime", 8, true},
                   {"restarts", 8, true}, {"memory", 10, true}};
        auto table_width = [&]() {
            std::size_t result = columns.size() * 3 + 1;
            for (const auto &column : columns) result += column.width;
            return result;
        };
        while (columns.size() > 3 && table_width() > terminal_width) {
            const std::string remove = columns.size() == 6 ? "memory" :
                                       columns.size() == 5 ? "uptime" : "restarts";
            columns.erase(std::remove_if(columns.begin(), columns.end(), [&](const column_spec &column) {
                return column.title == remove;
            }), columns.end());
        }
        const std::size_t without_name = table_width() - columns[0].width;
        columns[0].width = terminal_width > without_name + 4 ? terminal_width - without_name : 4;
        columns[0].width = std::min<std::size_t>(columns[0].width, 40);
    }

    ft_set_u8strwid_func(utf8_width_range);
    fort::utf8_table table;
    table.set_border_style(FT_BASIC_STYLE);
    table << fort::header;
    for (const auto &column : columns) table << column.title;
    table << fort::endr;

    auto cell_value = [](const process_list_entry &entry, const column_spec &column) {
        if (column.title == "name") return truncate_prefix(sanitize_table_text(entry.name), column.width);
        if (column.title == "pid") return entry.pid >= 0 ? std::to_string(entry.pid) : std::string("-");
        if (column.title == "state") return pm_state_to_str(entry.state);
        if (column.title == "uptime") return entry.has_uptime ? format_duration(entry.uptime_ms) : std::string("-");
        if (column.title == "restarts") return std::to_string(entry.restart_count);
        if (column.title == "retry") {
            if (entry.restart_suppressed) return std::string("suppressed");
            if (entry.restart_pending) return format_duration(entry.restart_delay_remaining_ms);
            return std::string("-");
        }
        if (column.title == "memory") return entry.has_rss ? format_memory(entry.rss_kib) : std::string("-");
        if (column.title == "log") return entry.log_degraded ? std::string("degraded") : std::string("ok");
        if (column.title == "daemon") return entry.daemon ? std::string("Y") : std::string("N");
        if (column.title == "pty") {
            if (entry.pty == pty_mode_t::unsupported) return std::string("-");
            return entry.pty == pty_mode_t::enabled ? std::string("Y") : std::string("N");
        }
        if (column.title == "depends_on") {
            return truncate_prefix(sanitize_table_text(join_dependencies(entry.depends_on)), column.width);
        }
        if (column.title == "cwd") return truncate_suffix(sanitize_table_text(entry.cwd), column.width);
        return truncate_prefix(sanitize_table_text(shell_preview(entry)), column.width);
    };

    for (const auto &entry : entries) {
        for (const auto &column : columns) table << cell_value(entry, column);
        table << fort::endr;
    }
    for (std::size_t column = 0; column < columns.size(); ++column) {
        table.column(column).set_cell_text_align(columns[column].right_align ?
                                                 fort::text_align::right : fort::text_align::left);
    }
    const bool color = options.stdout_is_tty && !options.no_color && std::getenv("NO_COLOR") == nullptr;
    if (color) {
        for (std::size_t row = 0; row < entries.size(); ++row) {
            const auto state_column = static_cast<std::size_t>(std::find_if(columns.begin(), columns.end(),
                [](const column_spec &column) { return column.title == "state"; }) - columns.begin());
            table.cell(row + 1, state_column).set_cell_content_fg_color(state_color(entries[row].state));
            table.cell(row + 1, state_column).set_cell_content_text_style(fort::text_style::bold);
        }
    }
    return "Total: " + std::to_string(entries.size()) + "\n" + table.to_string() + "\n";
}

std::size_t stdout_terminal_width() {
#ifdef _WIN32
    CONSOLE_SCREEN_BUFFER_INFO info{};
    HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
    if (output != INVALID_HANDLE_VALUE && GetConsoleScreenBufferInfo(output, &info)) {
        return static_cast<std::size_t>(info.srWindow.Right - info.srWindow.Left + 1);
    }
#else
    struct winsize size{};
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &size) == 0 && size.ws_col > 0) return size.ws_col;
#endif
    const char *columns = std::getenv("COLUMNS");
    if (columns != nullptr) {
        char *end = nullptr;
        const auto parsed = std::strtoul(columns, &end, 10);
        if (end != columns && *end == '\0' && parsed > 0) return static_cast<std::size_t>(parsed);
    }
    return 120;
}

bool stdout_supports_color() {
#ifdef _WIN32
    HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
    if (output == INVALID_HANDLE_VALUE || GetFileType(output) != FILE_TYPE_CHAR) return false;
    DWORD mode = 0;
    if (!GetConsoleMode(output, &mode)) return false;
    return SetConsoleMode(output, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING) != FALSE;
#else
    return isatty(STDOUT_FILENO) != 0;
#endif
}

} // namespace cli
} // namespace pm_tiny
