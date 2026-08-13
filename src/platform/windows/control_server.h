#pragma once

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "protocol_v2.h"

namespace pm_tiny {
namespace win {

struct ProcessHandle;
struct ProgramConfig;
struct RuntimeControlState;

class ControlServer {
public:
    ControlServer(std::vector<ProcessHandle> &processes,
                  std::mutex &process_mutex,
                  std::unordered_map<std::string, ProgramConfig> &config_map,
                  RuntimeControlState &runtime_state,
                  std::atomic_bool &should_stop_flag,
                  std::string config_path);
    ~ControlServer();

    ControlServer(const ControlServer &) = delete;
    ControlServer &operator=(const ControlServer &) = delete;

    bool start();
    void stop();

private:
    void run();
    pm_tiny::protocol_message handle_message(const pm_tiny::protocol_message &request);
    pm_tiny::protocol_message handle_list(const pm_tiny::protocol_message &request);
    std::string handle_stop(const std::string &name);
    std::string handle_restart(const std::string &name);
    std::string handle_start(const std::string &args);
    std::string handle_delete(const std::string &name);
    std::string handle_inspect(const std::string &name);
    std::string handle_log(const std::string &name);
    std::string handle_save();
    std::string handle_reload();
    std::string handle_app_signal(const std::string &name, bool ready_signal);

    std::wstring pipe_name_;
    std::vector<ProcessHandle> &processes_;
    std::mutex &process_mutex_;
    std::unordered_map<std::string, ProgramConfig> &config_map_;
    RuntimeControlState &runtime_state_;
    std::atomic_bool &should_stop_;
    std::string config_path_;

    std::atomic_bool running_{false};
    std::thread thread_;
};

} // namespace win
} // namespace pm_tiny
