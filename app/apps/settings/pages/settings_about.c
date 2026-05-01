#include "settings_about.h"
#include "settings_app.h"
#include "esp_log.h"
#include "lvgl.h"

#include "ui_tokens.h"
#include "ui_widgets.h"
#include "ui_statusbar.h"
#include "app_shell_ui.h"
#include "app_fonts.h"

static const char *TAG = "settings_about";

/* ============================================================================
 * 关于页 —— settings app 的子页
 *
 * 入口：settings_home 列表 → settings_app_push(SETTINGS_PAGE_ABOUT)
 * 退出：底部 50px 边缘上滑 → settings_app_pop_or_exit()
 * ========================================================================= */

#define EXIT_EDGE_HEIGHT  50

typedef struct {
    lv_obj_t *screen;
    lv_obj_t *statusbar;
    int       press_y0;
} ui_t;

static ui_t s_ui = { .press_y0 = -1 };

/* ============================================================================
 * 视图
 * ========================================================================= */

static void create_hero(lv_obj_t *parent, int top_y)
{
    lv_obj_t *ic = lv_label_create(parent);
    lv_label_set_text(ic, ICON_BLUETOOTH);
    lv_obj_set_style_text_font (ic, APP_FONT_ICONS_36, 0);
    lv_obj_set_style_text_color(ic, UI_C_ACCENT, 0);
    lv_obj_align(ic, LV_ALIGN_TOP_MID, 0, top_y);

    lv_obj_t *name = lv_label_create(parent);
    lv_label_set_text(name, "ESP32-S3 Demo");
    lv_obj_set_style_text_font (name, UI_F_TITLE, 0);
    lv_obj_set_style_text_color(name, UI_C_TEXT, 0);
    lv_obj_align(name, LV_ALIGN_TOP_MID, 0, top_y + 44);

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
    lv_obj_set_height(r1, 32);
    lv_obj_set_height(r2, 32);
    lv_obj_set_height(r3, 32);
    lv_obj_set_height(r4, 32);
}

/* ============================================================================
 * 手势：底缘 50px 上滑 pop
 * ========================================================================= */

static void on_pressed(lv_event_t *e)
{
    (void)e;
    lv_indev_t *indev = lv_indev_active();
    if (!indev) { s_ui.press_y0 = -1; return; }
    lv_point_t p; lv_indev_get_point(indev, &p);
    s_ui.press_y0 = p.y;
}

static void on_gesture(lv_event_t *e)
{
    (void)e;
    lv_indev_t *indev = lv_indev_active();
    if (!indev) return;
    lv_dir_t dir = lv_indev_get_gesture_dir(indev);
    if (dir != LV_DIR_TOP) { s_ui.press_y0 = -1; return; }

    int32_t screen_h = lv_obj_get_height(s_ui.screen);
    if (s_ui.press_y0 >= 0 && s_ui.press_y0 >= screen_h - EXIT_EDGE_HEIGHT) {
        settings_app_pop_or_exit();
    }
    s_ui.press_y0 = -1;
}

/* ============================================================================
 * 生命周期
 * ========================================================================= */

static lv_obj_t *create(void)
{
    ESP_LOGI(TAG, "create");

    s_ui.screen = lv_obj_create(NULL);
    ui_screen_setup(s_ui.screen);
    s_ui.press_y0 = -1;

    lv_obj_add_event_cb(s_ui.screen, on_pressed,  LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(s_ui.screen, on_gesture,  LV_EVENT_GESTURE, NULL);

    s_ui.statusbar = app_shell_attach_statusbar(s_ui.screen);
    create_hero(s_ui.screen, 24 + UI_SP_LG);
    create_info_card(s_ui.screen, 24 + UI_SP_LG + 96);

    return s_ui.screen;
}

static void destroy(void)
{
    ESP_LOGI(TAG, "destroy");
    if (s_ui.screen) {
        lv_obj_del(s_ui.screen);
        s_ui.screen = NULL;
    }
    s_ui.statusbar = NULL;
}

static const page_callbacks_t s_callbacks = {
    .create  = create,
    .destroy = destroy,
    .update  = NULL,
};

const page_callbacks_t *settings_about_get_callbacks(void)
{
    return &s_callbacks;
}
