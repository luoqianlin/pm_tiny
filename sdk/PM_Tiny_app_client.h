//
// Created by qianlinluo@foxmail.com on 23-9-7.
//

#ifndef PM_TINY_PM_TINY_APP_CLIENT_H
#define PM_TINY_PM_TINY_APP_CLIENT_H
#ifdef __cplusplus
#if __cplusplus
extern "C" {
#endif
#endif /* __cplusplus */
typedef void *PM_Tiny_Handle;

PM_Tiny_Handle PM_Tiny_Init();

int PM_Tiny_is_enable(PM_Tiny_Handle handle);

void PM_Tiny_get_app_name(PM_Tiny_Handle handle,char *name, int len);

void PM_Tiny_tick(PM_Tiny_Handle handle);

void PM_Tiny_ready(PM_Tiny_Handle handle);

void PM_Tiny_Destroy(PM_Tiny_Handle handle);
#ifdef __cplusplus
#if __cplusplus
}
#endif
#endif /* __cplusplus */
#endif //PM_TINY_PM_TINY_APP_CLIENT_H
