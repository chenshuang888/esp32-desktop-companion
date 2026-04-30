#include "page_about.h"
#include "esp_log.h"
#include "lvgl.h"

#include "ui_tokens.h"
#include "ui_widgets.h"
#include "ui_statusbar.h"
#include "app_fonts.h"

static const char *TAG = "page_about";

/* ============================================================================
 * 关于页 —— 列表风格
 *
 * 布局：
 *   状态栏 24px
 *   顶部 hero 区  —— 设备图标 + 名称 + 副标题
 *   信息卡片      —— Version / Device / Framework / GUI 四行 KV
 *
 * 入口：从 PAGE_SETTINGS 进入
 * 退出：底部 50px 边缘上滑回 PAGE_SETTINGS（与 weather 同样模式）
 * ========================================================================= */

typedef struct {
    lv_obj_t *screen;
    lv_obj_t *statusbar;
} page_about_ui_t;

static page_about_ui_t s_ui = {0};

/* ============================================================================
 * 视图层
 * ========================================================================= */

static void create_hero(lv_obj_t *parent, int top_y)
{
    /* 大图标 —— Material 36px 蓝牙 */
    lv_obj_t *ic = lv_label_create(parent);
    lv_label_set_text(ic, ICON_BLUETOOTH);
    lv_obj_set_style_text_font (ic, APP_FONT_ICONS_36, 0);
    lv_obj_set_style_text_color(ic, UI_C_ACCENT, 0);
    lv_obj_align(ic, LV_ALIGN_TOP_MID, 0, top_y);

    /* 标题 */
    lv_obj_t *name = lv_label_create(parent);
    lv_label_set_text(name, "ESP32-S3 Demo");
    lv_obj_set_style_text_font (name, UI_F_TITLE, 0);
    lv_obj_set_style_text_color(name, UI_C_TEXT, 0);
    lv_obj_align(name, LV_ALIGN_TOP_MID, 0, top_y + 44);

    /* 副标题 */
    lv_obj_t *sub = lv_label_create(parent);
    lv_label_set_text(sub, "BLE Time Sync");
    lv_obj_set_style_text_font (sub, UI_F_BODY, 0);
    lv_obj_set_style_text_color(sub, UI_C_TEXT_MUTED, 0);
    lv_obj_align(sub, LV_ALIGN_TOP_MID, 0, top_y + 66);
}

static void create_info_card(lv_obj_t *parent, int top_y)
{
    lv_obj_t *card = ui_card(parent);
    lv_obj_set_size(card, 216, 4 * 32 + 2 * UI_SP_SM);
    lv_obj_align(card, LV_ALIGN_TOP_MID, 0, top_y);

    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(card, 0, 0);
    lv_obj_set_style_pad_hor(card, UI_SP_MD, 0);
    lv_obj_set_style_pad_ver(card, UI_SP_SM, 0);

    lv_obj_t *r1 = ui_kv_row(card, "Version",   "v0.4",        NULL, true);
    lv_obj_t *r2 = ui_kv_row(card, "Device",    "ESP32-S3",    NULL, true);
    lv_obj_t *r3 = ui_kv_row(card, "Framework", "ESP-IDF 5.4", NULL, true);
    lv_obj_t *r4 = ui_kv_row(card, "GUI",       "LVGL 9.5",    NULL, false);
    /* 固定行高 32px，信息排列更舒展 */
    lv_obj_set_height(r1, 32);
    lv_obj_set_height(r2, 32);
    lv_obj_set_height(r3, 32);
    lv_obj_set_height(r4, 32);
}

static void on_screen_gesture(lv_event_t *e)
{
    (void)e;
    lv_indev_t *indev = lv_indev_active();
    if (!indev) return;
    lv_dir_t dir = lv_indev_get_gesture_dir(indev);
    if (dir == LV_DIR_TOP) {
        page_router_switch(PAGE_SETTINGS);
    }
}

/* ============================================================================
 * 生命周期
 * ========================================================================= */

static lv_obj_t *page_about_create(void)
{
    ESP_LOGI(TAG, "Creating about page");

    s_ui.screen = lv_obj_create(NULL);
    ui_screen_setup(s_ui.screen);

    lv_obj_add_event_cb(s_ui.screen, on_screen_gesture, LV_EVENT_GESTURE, NULL);

    s_ui.statusbar = ui_statusbar_create(s_ui.screen);
    create_hero(s_ui.screen, 24 + UI_SP_LG);
    create_info_card(s_ui.screen, 24 + UI_SP_LG + 96);

    return s_ui.screen;
}

static void page_about_destroy(void)
{
    ESP_LOGI(TAG, "Destroying about page");
    if (s_ui.screen) {
        lv_obj_del(s_ui.screen);
        s_ui.screen = NULL;
    }
    s_ui.statusbar = NULL;
}

static const page_callbacks_t s_callbacks = {
    .create  = page_about_create,
    .destroy = page_about_destroy,
    .update  = NULL,
};

const page_callbacks_t *page_about_get_callbacks(void)
{
    return &s_callbacks;
}
