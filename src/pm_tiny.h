//
// Created by luo on 2021/10/7.
//

#ifndef PM_TINY_PM_TINY_H
#define PM_TINY_PM_TINY_H

#define PM_TINY_PROG_STATE_NO_RUN 0
#define PM_TINY_PROG_STATE_RUNING 1
#define PM_TINY_PROG_STATE_STARTUP_FAIL 2
#define PM_TINY_PROG_STATE_REQUEST_STOP 3
#define PM_TINY_PROG_STATE_STOPED 4
#define PM_TINY_PROG_STATE_EXIT 5

#define PM_TINY_PROG_STATE_STARTING 6
#define PM_TINY_PROG_STATE_WAITING_START 7
#define PM_TINY_PROG_STATE_REQUEST_DELETE 8

#define PM_TINY_FRAME_TYPE_SHOW_LOG 0x30
#define PM_TINY_FRAME_TYPE_APP_READY 0x31
#define PM_TINY_FRAME_TYPE_APP_TICK 0x32
#define PM_TINY_FRAME_TYPE_APP_INSPECT 0x33
#define PM_TINY_FRAME_TYPE_RELOAD 0x34
#define PM_TINY_FRAME_TYPE_QUIT 0x35

#define PM_TINY_HOME "PM_TINY_HOME"
#define PM_TINY_LOG_FILE "PM_TINY_LOG_FILE"
#define PM_TINY_SOCK_FILE "PM_TINY_SOCK_FILE"
#define PM_TINY_PROG_CFG_FILE "PM_TINY_PROG_CFG_FILE"
#define PM_TINY_APP_LOG_DIR "PM_TINY_APP_LOG_DIR"
#define PM_TINY_APP_ENVIRON_DIR "PM_TINY_APP_ENVIRON_DIR"

#define PM_TINY_DEFAULT_CFG_FILE "/usr/local/pm_tiny/pm_tiny.yaml"
#define PM_TINY_APP_NAME "PM_TINY_APP_NAME"
#define PM_TINY_UDS_ABSTRACT_NAMESPACE "PM_TINY_UDS_ABSTRACT_NAMESPACE"

#define PM_TINY_EXPORTS __attribute__ ((visibility ("default")))

inline std::string pm_state_to_str(int state) {
    switch (state) {
        case PM_TINY_PROG_STATE_NO_RUN:
            return "offline";
        case PM_TINY_PROG_STATE_RUNING:
            return "online";
        case PM_TINY_PROG_STATE_STARTUP_FAIL:
            return "startfail";
        case PM_TINY_PROG_STATE_REQUEST_STOP:
            return "stoping";
        case PM_TINY_PROG_STATE_STOPED:
            return "stoped";
        case PM_TINY_PROG_STATE_EXIT:
            return "exit";
        case PM_TINY_PROG_STATE_STARTING:
            return "starting";
        case PM_TINY_PROG_STATE_WAITING_START:
            return "waiting";
        case PM_TINY_PROG_STATE_REQUEST_DELETE:
            return "deleting";
        default:
            return "Unkown";
    }
}
#endif //PM_TINY_PM_TINY_H
