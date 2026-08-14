#include <windows.h>

#include <atomic>
#include <string>
#include <vector>

namespace {

HANDLE stop_event = nullptr;

BOOL WINAPI control_handler(DWORD type) {
    if (type == CTRL_BREAK_EVENT && stop_event != nullptr) {
        SetEvent(stop_event);
        return TRUE;
    }
    return FALSE;
}

std::wstring quote(const std::wstring &value) {
    return L"\"" + value + L"\"";
}

bool write_text(const std::wstring &path, const std::string &content, bool append) {
    HANDLE file = CreateFileW(path.c_str(),
                              append ? FILE_APPEND_DATA : GENERIC_WRITE,
                              FILE_SHARE_READ | FILE_SHARE_WRITE,
                              nullptr,
                              append ? OPEN_ALWAYS : CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL,
                              nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const bool success = WriteFile(file, content.data(), static_cast<DWORD>(content.size()), &written, nullptr) &&
                         written == content.size();
    CloseHandle(file);
    return success;
}

void append_marker(const std::wstring &path, const char *role) {
    write_text(path, std::string(role) + "=" + std::to_string(GetCurrentProcessId()) + "\n", true);
}

} // namespace

int wmain(int argc, wchar_t *argv[]) {
    bool child = false;
    std::wstring mode = L"graceful";
    std::wstring pid_file;
    std::wstring marker_file;
    for (int i = 1; i < argc; ++i) {
        const std::wstring arg = argv[i];
        if (arg == L"--child") child = true;
        else if (arg == L"--mode" && i + 1 < argc) mode = argv[++i];
        else if (arg == L"--pid-file" && i + 1 < argc) pid_file = argv[++i];
        else if (arg == L"--marker-file" && i + 1 < argc) marker_file = argv[++i];
    }
    if (pid_file.empty() || marker_file.empty()) return 2;

    stop_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (stop_event == nullptr || !SetConsoleCtrlHandler(control_handler, TRUE)) return 3;

    PROCESS_INFORMATION child_process{};
    if (!child) {
        wchar_t executable[MAX_PATH]{};
        if (GetModuleFileNameW(nullptr, executable, MAX_PATH) == 0) return 4;
        std::wstring command = quote(executable) + L" --child --mode " + mode +
                               L" --pid-file " + quote(pid_file) +
                               L" --marker-file " + quote(marker_file);
        std::vector<wchar_t> mutable_command(command.begin(), command.end());
        mutable_command.push_back(L'\0');
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        if (!CreateProcessW(nullptr, mutable_command.data(), nullptr, nullptr, FALSE, 0,
                            nullptr, nullptr, &startup, &child_process)) {
            return 5;
        }
        const std::wstring ready_file = marker_file + L".ready";
        for (int attempt = 0; attempt < 50 && GetFileAttributesW(ready_file.c_str()) == INVALID_FILE_ATTRIBUTES;
             ++attempt) {
            Sleep(100);
        }
        const std::string pids = std::to_string(GetCurrentProcessId()) + "\n" +
                                 std::to_string(child_process.dwProcessId) + "\n";
        if (!write_text(pid_file, pids, false)) return 6;
    }

    if (child) append_marker(marker_file + L".ready", "child-ready");

    WaitForSingleObject(stop_event, INFINITE);
    const bool resistant = mode == L"resistant" || (mode == L"root-first" && child);
    if (resistant) {
        Sleep(INFINITE);
    }

    append_marker(marker_file + (child ? L".child" : L".parent"), child ? "child" : "parent");
    if (!child) Sleep(300);
    if (child_process.hThread != nullptr) CloseHandle(child_process.hThread);
    if (child_process.hProcess != nullptr) CloseHandle(child_process.hProcess);
    CloseHandle(stop_event);
    return 0;
}
