//
// Created by qianlinluo@foxmail.com on 23-7-27.
//

#ifndef PM_TINY_PM_FUNCS_H
#define PM_TINY_PM_FUNCS_H

#include "session.h"
#include <signal.h>
#include "pm_tiny_enum.h"
#include "dependency_graph_renderer.h"
#include "process_list_renderer.h"
#include "daemon_info_renderer.h"
#include "protocol_v3.h"

namespace pm_funcs {
    bool display_daemon_info(pm_tiny::session_t &session, bool json);
    bool display_proc_infos(pm_tiny::session_t &session,
                            const pm_tiny::cli::list_render_options &options = {});

    bool display_dependency_graph(pm_tiny::session_t &session,
                                  const pm_tiny::cli::dependency_graph_render_options &options = {});

    bool stop_proc(pm_tiny::session_t &session, const std::string &app_name, bool no_list = false);

    bool start_proc(pm_tiny::session_t &session, const pm_tiny::start_request &request);

    bool save_proc(pm_tiny::session_t &session);

    bool delete_prog(pm_tiny::session_t &session, const std::string &app_name, bool no_list = false);

    bool restart_prog(pm_tiny::session_t &session, const std::string &app_name
                      ,bool show_log, bool no_list = false);

    bool show_version(pm_tiny::session_t &session);

    bool show_prog_log(pm_tiny::session_t &session, const std::string &app_name, bool history);

    bool loop_read_show_process_log(pm_tiny::session_t &session);

    bool inspect_proc(pm_tiny::session_t &session, const std::string &app_name);

    bool pm_tiny_quit(pm_tiny::session_t &session);

    bool pm_tiny_reload(pm_tiny::session_t &session, int extra, bool no_list = false);
}
#endif //PM_TINY_PM_FUNCS_H
