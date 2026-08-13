
#ifndef PM_TINY_PM_FUNCS_H
#define PM_TINY_PM_FUNCS_H

#include "session.h"
#include <signal.h>
#include "pm_tiny_enum.h"
#include "dependency_graph_renderer.h"
#include "process_list_renderer.h"

namespace pm_funcs {
    struct progcfg_t {
        std::string name;
        std::string work_dir;
        std::string command;
        int daemon{};
        std::vector<std::string> depends_on;
        int start_timeout{};
        pm_tiny::failure_action_t failure_action;
        int heartbeat_timeout{};
        int kill_timeout_sec{};
        std::string run_as;
        int oom_score_adj;
        std::vector<std::string> env_vars;
        bool pty = true;
        int restart_delay_ms = 1000;
        int restart_max_delay_ms = 30000;
        int restart_window_ms = 60000;
        int restart_max_attempts = 10;
        int restart_reset_after_ms = 60000;
        bool restart_pending = false;
        std::int64_t restart_delay_remaining_ms = 0;
        int restart_attempts_in_window = 0;
        bool restart_suppressed = false;
        std::string restart_suppression_reason;

        void read(pm_tiny::iframe_stream &ifs);

        void show();
    };

    void display_proc_infos(pm_tiny::session_t &session,
                            const pm_tiny::cli::list_render_options &options = {});

    bool display_dependency_graph(pm_tiny::session_t &session,
                                  const pm_tiny::cli::dependency_graph_render_options &options = {});

    void stop_proc(pm_tiny::session_t &session, const std::string &app_name);

    void start_proc(pm_tiny::session_t &session,
                    const progcfg_t &prog_cfg, bool show_log);

    void save_proc(pm_tiny::session_t &session);

    void delete_prog(pm_tiny::session_t &session, const std::string &app_name);

    void restart_prog(pm_tiny::session_t &session, const std::string &app_name
                      ,bool show_log);

    void show_version(pm_tiny::session_t &session);

    void show_prog_log(pm_tiny::session_t &session, const std::string &app_name);

    void loop_read_show_process_log(pm_tiny::session_t &session);

    void show_msg(int code, const std::string &msg);

    void inspect_proc(pm_tiny::session_t &session, const std::string &app_name);

    bool pm_tiny_quit(pm_tiny::session_t &session);

    void pm_tiny_reload(pm_tiny::session_t &session, int extra);
}
#endif //PM_TINY_PM_FUNCS_H
