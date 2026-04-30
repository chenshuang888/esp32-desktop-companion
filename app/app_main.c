#include "app_main.h"
#include "pages/page_time.h"
#include "pages/page_time_adjust.h"
#include "pages/page_menu.h"
#include "pages/page_about.h"
#include "pages/page_settings.h"
#include "pages/page_weather.h"
#include "pages/page_notifications.h"
#include "pages/page_music.h"
#include "pages/page_system.h"
#include "pages/page_dynamic_app.h"
#include "page_router.h"
#include "app_fonts.h"
#include "weather_icons.h"
#include "lvgl_port.h"
#include "lcd_panel.h"
#include "time_manager.h"
#include "weather_manager.h"
#include "notify_manager.h"
#include "media_manager.h"
#include "system_manager.h"
#include "backlight_storage.h"
#include "time_storage.h"
#include "dynamic_app.h"
#include "dynamic_app_registry.h"
#include "dynapp_fs_worker.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "app_main";

/* 运行中检测 hook：fs_worker 在删脚本前调一次。
 * 跑在 fs_worker task，仅读取 dynamic_app_registry 静态字段，安全。
 * page_router_get_current 也是纯读，不动 LVGL。 */
static bool is_app_running(const char *name)
{
    if (page_router_get_current() != PAGE_DYNAMIC_APP) return false;
    const char *running = dynamic_app_registry_current();
    return running && running[0] && strcmp(running, name) == 0;
}

static void ui_task(void *arg)
{
    while (1) {
        /*
         * Script -> UI 队列桥接：
         * - 脚本任务里调用 sys.ui.setText(...) 时，只是把“更新请求”塞进队列；
         * - UI 任务在这里 drain 队列，才能真正调用 LVGL 更新控件。
         *
         * 放在 page_router_update/lvgl_port_task_handler 之前的原因：
         * - 尽量让“这一帧收到的更新”能在同一帧渲染出来，减少肉眼可见的延迟。
         */
        /* Script -> UI 队列桥：每帧消化较多命令，避免脚本一次性 build UI 时被丢弃 */
        dynamic_app_ui_drain(32);

        time_manager_process_pending();
        weather_manager_process_pending();
        notify_manager_process_pending();
        media_manager_process_pending();
        system_manager_process_pending();
        page_router_update();
        lvgl_port_task_handler();

        /* 周期性持久化（内部自判是否到时间/dirty，绝大多数 tick 即返回） */
        notify_manager_tick_flush();
        time_storage_tick_save();

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

esp_err_t app_main_init(void)
{
    ESP_LOGI(TAG, "Initializing application");

    ESP_ERROR_CHECK(lvgl_port_init());

    /* 初始化中文字体副本（带 fallback 链），必须在首个页面 create 之前 */
    app_fonts_init();

    /* 初始化天气图标资源（解析 EMBED 进来的 8 张 .bin 为 lv_image_dsc_t） */
    weather_icons_init();

    /* 恢复上次背光亮度（lvgl_port_init 内部已初始化 lcd_panel） */
    lcd_panel_set_backlight(backlight_storage_get());

    /*
     * Dynamic App 运行时初始化（MicroQuickJS MVP）：
     * - 创建脚本任务（通常固定在 Core0）
     * - 初始化 Script->UI 队列
     *
     * 注意：dynamic_app_start() 是在页面里调用的，但前提是这里先 init 成功。
     */
    ESP_ERROR_CHECK(dynamic_app_init());

    ESP_ERROR_CHECK(page_router_init());

    ESP_ERROR_CHECK(page_router_register(PAGE_TIME, page_time_get_callbacks()));
    ESP_ERROR_CHECK(page_router_register(PAGE_MENU, page_menu_get_callbacks()));
    ESP_ERROR_CHECK(page_router_register(PAGE_ABOUT, page_about_get_callbacks()));
    ESP_ERROR_CHECK(page_router_register(PAGE_WEATHER, page_weather_get_callbacks()));
    ESP_ERROR_CHECK(page_router_register(PAGE_NOTIFICATIONS, page_notifications_get_callbacks()));
    ESP_ERROR_CHECK(page_router_register(PAGE_MUSIC, page_music_get_callbacks()));
    ESP_ERROR_CHECK(page_router_register(PAGE_TIME_ADJUST, page_time_adjust_get_callbacks()));
    ESP_ERROR_CHECK(page_router_register(PAGE_SETTINGS, page_settings_get_callbacks()));
    ESP_ERROR_CHECK(page_router_register(PAGE_SYSTEM, page_system_get_callbacks()));
    ESP_ERROR_CHECK(page_router_register(PAGE_DYNAMIC_APP, page_dynamic_app_get_callbacks()));

    ESP_ERROR_CHECK(page_router_switch(PAGE_TIME));

    /* 注入"运行中检测"hook，让 fs_worker 拒绝删除当前正在跑的 app。
     * 必须在 page_router_init 之后注册（hook 内部读 page_router_get_current）。 */
    dynapp_fs_worker_set_running_check(is_app_running);

    /*
     * UI 任务固定在 Core1：
     * - 与脚本任务（Core0）分开，减少抢占；
     * - 并且方便建立“UI 相关操作只能在 UI 任务做”的心智模型。
     */
    BaseType_t ret = xTaskCreatePinnedToCore(ui_task, "ui_task", 8192, NULL, 5, NULL, 1);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create UI task");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Application initialized");
    return ESP_OK;
}
