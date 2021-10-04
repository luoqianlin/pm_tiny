//
// Created by qianlinluo@foxmail.com on 23-7-27.
//

#ifndef PM_TINY_PM_TINY_FUNCS_H
#define PM_TINY_PM_TINY_FUNCS_H

#include "pm_tiny_server.h"
#include "prog.h"

std::string msg_cmd_not_completed(const std::string &name);

std::string msg_DAG_not_completed(const std::string &name);

std::string msg_server_stoping();

pm_tiny::frame_ptr_t make_server_stoping_frame();

pm_tiny::frame_ptr_t make_server_reloading_frame();

pm_tiny::frame_ptr_t make_prog_info_data(pm_tiny::proglist_t &pm_tiny_progs);

std::unique_ptr<pm_tiny::frame_t>
handle_cmd_start(pm_tiny::pm_tiny_server_t &pm_tiny_server, pm_tiny::iframe_stream &ifs,
                 std::shared_ptr<pm_tiny::session_t> &session);

void handle_cmd_inspect(pm_tiny::pm_tiny_server_t &pm_tiny_server,
                        pm_tiny::iframe_stream &ifs,
                        std::shared_ptr<pm_tiny::session_t> &session);

void handle_frame(pm_tiny::pm_tiny_server_t &pm_tiny_server, pm_tiny::frame_ptr_t &rf,
                  pm_tiny::session_ptr_t &session);

void prog_bind_session(pm_tiny::session_ptr_t &session,
                       const pm_tiny::prog_ptr_t &prog, pm_tiny::frame_ptr_t &wf);

#endif //PM_TINY_PM_TINY_FUNCS_H
