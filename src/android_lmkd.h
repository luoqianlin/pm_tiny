//
// Created by qianlinluo@foxmail.com on 24-7-31.
//

#ifndef PM_TINY_ANDROID_LMKD_H
#define PM_TINY_ANDROID_LMKD_H
#include "pm_tiny_utility.h"
namespace pm_tiny {
    constexpr int OOM_SCORE_ADJ_MIN  = -1000;
    constexpr int  OOM_SCORE_ADJ_MAX = 1000;
    enum class lmk_cmd  {
        LMK_TARGET = 0, /* Associate minfree with oom_adj_score */
        LMK_PROCPRIO,   /* Register a process and set its oom_adj_score */
        LMK_PROCREMOVE, /* Unregister a process */
        LMK_PROCPURGE,  /* Purge all registered processes */
        LMK_GETKILLCNT, /* Get number of kills */
        LMK_SUBSCRIBE,  /* Subscribe for asynchronous events */
        LMK_PROCKILL,   /* Unsolicited msg to subscribed clients on proc kills */
        LMK_UPDATE_PROPS, /* Reinit properties */
    };
/* Process types for lmk_procprio.ptype */
    enum class proc_type {
        PROC_TYPE_FIRST,
        PROC_TYPE_APP = PROC_TYPE_FIRST,
        PROC_TYPE_SERVICE,
        PROC_TYPE_COUNT,
    };

    CloseableFd connect_lmkd();

    int lmk_procprio(int sock, pid_t pid, uid_t uid, int oom_score_adj);
    int cmd_procremove(int sock, pid_t pid);
}
#endif //PM_TINY_ANDROID_LMKD_H
