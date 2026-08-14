#pragma once

#include <atomic>
#include <string>
#include <map>
#include <vector>
#include <memory>
#include <fstream>

#include "protocol_v3.h"
#include "persistence_worker.h"
#include "win_config_loader.h"
#include "daemon_config.h"
#include "daemon_info.h"

namespace pm_tiny {
namespace win {

struct ProcessHandle;
struct RuntimeControlState;
class AsyncNamedPipeSession;
class AsyncNamedPipeServer;

class ControlServer {
public:
    ControlServer(std::vector<ProcessHandle> &processes,
                  std::map<std::string, ProgramConfig> &config_map,
                  RuntimeControlState &runtime_state,
                  std::atomic_bool &should_stop_flag,
                  std::string program_config_path,
                  std::string app_environ_dir,
                  std::string app_log_dir,
                  daemon_config daemon_configuration,
                  daemon_cli_options daemon_options,
                  unsigned long long started_ms);
    ~ControlServer();

    ControlServer(const ControlServer &) = delete;
    ControlServer &operator=(const ControlServer &) = delete;

    bool start();
    void poll();
    void run_for(unsigned long milliseconds);
    void stop();
    bool persistence_busy() const;
    void refresh_process_exit_watches();

private:
    void handle_async_request(const std::shared_ptr<AsyncNamedPipeSession> &session,
                              const protocol_message &request);
    void poll_pending_responses();
    void poll_log_streams();
    void handle_process_exit(const std::string &name, unsigned long long generation,
                             unsigned long exit_code);
    void handle_process_log(const std::string &name, unsigned long long generation, int stream_index,
                            const char *data, std::size_t size, bool eof);
    void begin_log_stream(const std::shared_ptr<AsyncNamedPipeSession> &session,
                          const protocol_message &request, const std::string &name,
                          bool follow_process = false);
    pm_tiny::protocol_message handle_message(const pm_tiny::protocol_message &request);
    pm_tiny::protocol_message handle_list(const pm_tiny::protocol_message &request);
    pm_tiny::protocol_message handle_info(const pm_tiny::protocol_message &request);
    std::string handle_stop(const std::string &name);
    std::string handle_restart(const std::string &name);
    pm_tiny::protocol_message handle_start(const pm_tiny::protocol_message &message,
                                           const pm_tiny::start_request &request);
    std::string handle_delete(const std::string &name);
    pm_tiny::protocol_message handle_inspect(const pm_tiny::protocol_message &request,
                                             const std::string &name);
    std::string handle_log(const std::string &name);
    std::string handle_save();
    std::string handle_reload();
    std::string handle_app_signal(const std::string &name, bool ready_signal);

    std::wstring pipe_name_;
    std::vector<ProcessHandle> &processes_;
    std::map<std::string, ProgramConfig> &config_map_;
    RuntimeControlState &runtime_state_;
    std::atomic_bool &should_stop_;
    std::string program_config_path_;
    std::string app_environ_dir_;
    std::string app_log_dir_;
    daemon_config daemon_configuration_;
    daemon_cli_options daemon_options_;
    unsigned long long started_ms_ = 0;

    std::atomic_bool running_{false};
    std::unique_ptr<AsyncNamedPipeServer> server_;
    struct pending_response;
    std::vector<pending_response> pending_responses_;
    struct log_stream;
    std::vector<log_stream> log_streams_;
    persistence_worker persistence_;
};

} // namespace win
} // namespace pm_tiny
