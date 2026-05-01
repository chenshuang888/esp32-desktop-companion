#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * app_shell_ui —— app 层的 UI helper（依赖 app/ui 组件）
 *
 * 与 framework/app_shell.h 的分工：
 *   framework/app_shell.h —— 纯路由 helper（不依赖任何 UI）
 *   app/ui/app_shell_ui.h —— 需要 ui_statusbar 等组件的 helper
 *
 * 用法（非沉浸式 app on_enter）：
 *   lv_obj_t *screen = lv_obj_create(NULL);
 *   ui_screen_setup(screen);
 *   app_shell_attach_statusbar(screen);  // 顶部 24px statusbar
 *   ... 其它内容 align 到 24px 以下
 * ========================================================================= */

/**
 * 在 screen 顶部挂统一 24px 状态栏。返回 statusbar obj。
 * 内部就是 ui_statusbar_create 的语义化包装，方便后续替换实现。
 */
lv_obj_t *app_shell_attach_statusbar(lv_obj_t *screen);

#ifdef __cplusplus
}
#endif
