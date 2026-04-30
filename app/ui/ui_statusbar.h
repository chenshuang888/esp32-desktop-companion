#pragma once

/* ============================================================================
 * ui_statusbar —— 系统级顶部状态栏（高 24px）
 *
 * 布局：
 *   [ 14:32 ]                    [ 蓝牙图标 ] [ 电池图标 87% ]
 *      左                                          右
 *
 * 数据：
 *   时间    : time(NULL) + localtime_r           (1Hz 自更新)
 *   蓝牙    : ble_driver_is_connected()           (1Hz 自更新)
 *   电池    : battery_sim_get_percent/state       (1Hz 自更新)
 *
 * 用法：
 *   lv_obj_t *bar = ui_statusbar_create(parent);
 *   // 自动占满 parent 顶部 240×24，不需要 align
 *
 * 销毁：parent 删除时一起销毁；内部 timer 自动停（绑定到 obj 的 delete cb）
 * ========================================================================= */

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* parent 必须是 240 宽的容器（如 screen）。
 * 返回容器对象（顶部 24px 高）。 */
lv_obj_t *ui_statusbar_create(lv_obj_t *parent);

#ifdef __cplusplus
}
#endif
