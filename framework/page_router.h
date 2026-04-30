#pragma once

#include "esp_err.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 页面回调（最小契约：create/destroy/update）。
 *
 * page_router 的职责是“管理页面生命周期”：
 * - 切到某个页面时：调用该页面的 `create()` 创建一个新的 LVGL screen/root，并加载显示
 * - 在 UI 循环中：周期性调用当前页面的 `update()`（如果提供）
 * - 切走/销毁页面时：调用页面的 `destroy()`（如果提供）来释放页面私有资源
 *
 * 线程约束（重要）：
 * - 这些回调都应在 UI 线程执行，因为它们会创建/操作 LVGL 对象。
 */
typedef struct {
    /**
     * 创建页面并返回 screen/root（通常为 `lv_obj_create(NULL)`）。
     *
     * @return 新建的 screen/root；必须是一个独立的 screen，而不是 `lv_scr_act()`。
     */
    lv_obj_t *(*create)(void);

    /**
     * 销毁页面私有资源（可选）。
     *
     * 说明：
     * - 如果页面需要释放“非 LVGL 对象”的资源（例如定时器、任务、队列、缓冲区），请实现 destroy；
     * - 若未实现 destroy，router 会对该页面 screen 执行 `lv_obj_del()`。
     */
    void (*destroy)(void);

    /**
     * 页面周期性更新入口（可选）。
     *
     * 说明：
     * - 只会在 UI 线程调用；
     * - 适合用来刷新动态信息（时间、状态、动画等）。
     */
    void (*update)(void);
} page_callbacks_t;

/**
 * 页面 ID。
 */
typedef enum {
    PAGE_TIME,           // 锁屏时间页
    PAGE_MENU,           // 菜单页
    PAGE_ABOUT,          // 关于页
    PAGE_WEATHER,        // 天气页
    PAGE_NOTIFICATIONS,  // 通知页
    PAGE_MUSIC,          // 音乐页（曲目展示 + 播放控制）
    PAGE_TIME_ADJUST,    // 时间/日期调整页（从设置进入）
    PAGE_SETTINGS,       // 设置页（聚合时间调节/亮度/关于）
    PAGE_SYSTEM,         // 系统页（CPU/MEM/DISK/BAT/NET/Temp）
    PAGE_DYNAMIC_APP,    // 动态 App 演示页（MicroQuickJS MVP）
    PAGE_MAX,            // 页面总数（自动计算）
} page_id_t;

/**
 * 初始化页面路由器。
 */
esp_err_t page_router_init(void);

/**
 * 注册页面回调。
 *
 * @param id 页面 ID
 * @param callbacks 页面回调（create 必须非空）
 */
esp_err_t page_router_register(page_id_t id, const page_callbacks_t *callbacks);

/**
 * 切换页面。
 *
 * @note 通常在 UI 线程调用。
 */
esp_err_t page_router_switch(page_id_t id);

/**
 * 用一个 *已经在外面 prepare 好的* screen 接管为当前页，跳过该页的 create()。
 *
 * 用法场景：希望"目标页对象树在后台先建好，再瞬间切屏"以消除组装过程的中间帧。
 * 标准流程：
 *   1) 调用方自己 lv_obj_create(NULL) + 填充子树（不 lv_scr_load）
 *   2) 把这棵 screen 传进来
 *   3) 本函数销毁旧页 + lv_scr_load(prepared_screen) + 更新内部状态
 *
 * 与 page_router_switch 的差异：
 *   - 不调 callbacks->create
 *   - 其它（destroy 旧页、状态更新）完全一致
 *
 * 失败时（id 非法 / prepared_screen 为 NULL / 当前已在该页）返回错误码，
 * 调用方需要自己释放 prepared_screen。
 *
 * @note 只在 UI 线程调用。
 */
esp_err_t page_router_commit_prepared(page_id_t id, lv_obj_t *prepared_screen);

/**
 * 获取当前页面 ID。
 */
page_id_t page_router_get_current(void);

/**
 * UI 线程周期性调用：驱动当前页面的 `update()`。
 */
void page_router_update(void);

#ifdef __cplusplus
}
#endif

