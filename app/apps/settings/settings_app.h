#pragma once

#include "app_router.h"

/* 设置 app —— 多 page，内部用 sub_router 串起 home/time/about */
extern const app_descriptor_t SETTINGS_APP;

/* ---- 给子页用的内部导航 API ----
 * 子页（settings_time / settings_about）上滑时调 settings_app_pop_or_exit()，
 * 由 settings_app 决定 sub_router_pop 还是 app_router_exit_to_launcher。
 *
 * 子页之间不直接 include sub_router 实例，避免硬耦合。
 */

typedef enum {
    SETTINGS_PAGE_HOME = 0,
    SETTINGS_PAGE_TIME,
    SETTINGS_PAGE_ABOUT,
} settings_page_id_t;

void settings_app_push(settings_page_id_t id);
void settings_app_pop_or_exit(void);   /* 栈空则 exit_to_launcher */
