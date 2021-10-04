//
// Created by qianlinluo@foxmail.com on 23-7-27.
//

#ifndef PM_TINY_PM_FUNCS_H
#define PM_TINY_PM_FUNCS_H

#include "session.h"
#include <signal.h>
#include "pm_tiny_enum.h"

namespace pm_funcs {
    struct proc_info_t {
        int pid;
        std::string name;
        std::string work_dir;
        std::string command;
        int restart_count;
        int state;
        long long vm_rss_kib;
        bool daemon;
        std::vector<std::string> depends_on;
    };

    inline std::ostream &operator<<(std::ostream &os, proc_info_t const &p) {
        os << p.pid << " " << p.name << " "
           << p.work_dir << " " << p.command << " "
           << p.restart_count << " " << p.state;
        return os;
    }

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

        void read(pm_tiny::iframe_stream &ifs);

        void show();
    };

    void display_proc_infos(pm_tiny::session_t &session);

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

    void pm_tiny_quit(pm_tiny::session_t &session);

    void pm_tiny_reload(pm_tiny::session_t &session, int extra);
}
#endif //PM_TINY_PM_FUNCS_H
