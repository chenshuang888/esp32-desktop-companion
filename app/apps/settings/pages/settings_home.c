#include "settings_home.h"
#include "settings_app.h"

#include "esp_log.h"
#include "lvgl.h"

#include "ui_tokens.h"
#include "ui_widgets.h"
#include "ui_statusbar.h"
#include "app_shell_ui.h"
#include "app_fonts.h"

#include "lcd_panel.h"
#include "backlight_storage.h"

static const char *TAG = "settings_home";

/* ============================================================================
 * 设置页 —— 列表风格
 *
 * 布局（240×320）：
 *   状态栏 24px        ui_statusbar
 *   标题区  ~40px      "设置" 16px 大标题，左对齐
 *   列表卡片 ~152px    ui_card 包 3 行 ui_list_row（48 ×3 = 144）
 *
 * 行：
 *   1) 时间调节  → settings_app_push(SETTINGS_PAGE_TIME)
 *   2) 亮度      → 点击循环档位（不进入子页），右值显示档位文字
 *   3) 关于      → settings_app_push(SETTINGS_PAGE_ABOUT)
 *
 * 手势：
 *   底缘 50px 上滑 → settings_app_pop_or_exit()（home 处栈空 → exit_to_launcher）
 * ========================================================================= */

/* 背光四档（与原 page_menu 中保持一致）*/
static const uint8_t   BACKLIGHT_STEPS[] = {64, 128, 192, 255};
static const char     *BACKLIGHT_LABELS[] = {"低", "中", "高", "满"};
#define BACKLIGHT_STEP_COUNT (int)(sizeof(BACKLIGHT_STEPS) / sizeof(BACKLIGHT_STEPS[0]))

typedef struct {
    lv_obj_t *screen;
    lv_obj_t *statusbar;
    lv_obj_t *bright_value;   /* 亮度行右侧 value label，点击后实时刷新 */
} page_settings_ui_t;

static page_settings_ui_t s_ui = {0};

/* ============================================================================
 * 行为
 * ========================================================================= */

static int current_backlight_idx(void)
{
    uint8_t cur = lcd_panel_get_backlight();
    int idx = 0;
    for (int i = 0; i < BACKLIGHT_STEP_COUNT; i++) {
        if (cur <= BACKLIGHT_STEPS[i]) { idx = i; break; }
        idx = i;
    }
    return idx;
}

static void cycle_backlight(void)
{
    int idx  = current_backlight_idx();
    int next = (idx + 1) % BACKLIGHT_STEP_COUNT;
    uint8_t duty = BACKLIGHT_STEPS[next];
    backlight_storage_set(duty);
    lcd_panel_set_backlight(duty);
    if (s_ui.bright_value) {
        lv_label_set_text(s_ui.bright_value, BACKLIGHT_LABELS[next]);
    }
    ESP_LOGI(TAG, "Backlight -> %d (%s)", duty, BACKLIGHT_LABELS[next]);
}

/* ============================================================================
 * 事件
 * ========================================================================= */

static void on_row_time_clicked(lv_event_t *e)
{
    (void)e;
    settings_app_push(SETTINGS_PAGE_TIME);
}

static void on_row_bright_clicked(lv_event_t *e)
{
    (void)e;
    cycle_backlight();
}

static void on_row_about_clicked(lv_event_t *e)
{
    (void)e;
    settings_app_push(SETTINGS_PAGE_ABOUT);
}

/* 底部 50px 边缘上滑退出 —— 与 weather 同样模式，避免误触列表行 */
#define EXIT_EDGE_HEIGHT  50
static int s_press_y0 = -1;

static void on_screen_pressed(lv_event_t *e)
{
    (void)e;
    lv_indev_t *indev = lv_indev_active();
    if (!indev) { s_press_y0 = -1; return; }
    lv_point_t p;
    lv_indev_get_point(indev, &p);
    s_press_y0 = p.y;
}

static void on_screen_gesture(lv_event_t *e)
{
    (void)e;
    lv_indev_t *indev = lv_indev_active();
    if (!indev) return;
    lv_dir_t dir = lv_indev_get_gesture_dir(indev);
    if (dir != LV_DIR_TOP) { s_press_y0 = -1; return; }

    int32_t screen_h = lv_obj_get_height(s_ui.screen);
    if (s_press_y0 >= 0 && s_press_y0 >= screen_h - EXIT_EDGE_HEIGHT) {
        settings_app_pop_or_exit();   /* home 处栈空 → exit_to_launcher */
    }
    s_press_y0 = -1;
}

/* ============================================================================
 * 视图层
 * ========================================================================= */

static void create_title(lv_obj_t *parent)
{
    lv_obj_t *title = lv_label_create(parent);
    lv_label_set_text(title, "设置");
    lv_obj_set_style_text_font (title, UI_F_TITLE, 0);
    lv_obj_set_style_text_color(title, UI_C_TEXT, 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, UI_SP_LG, 24 + UI_SP_MD);
}

static void create_list_card(lv_obj_t *parent)
{
    lv_obj_t *card = ui_card(parent);
    lv_obj_set_size(card, 216, 48 * 3);
    lv_obj_align(card, LV_ALIGN_TOP_MID, 0, 24 + UI_SP_MD + 16 + UI_SP_SM);

    /* card 自带 12px padding，列表行需要顶到边 */
    lv_obj_set_style_pad_all(card, 0, 0);
    lv_obj_set_flex_flow (card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(card, 0, 0);

    /* 行 1: 时间调节 */
    lv_obj_t *row_time = ui_list_row(card,
        ICON_EDIT_CALENDAR, "时间调节", NULL,
        UI_C_INFO, NULL);
    lv_obj_add_event_cb(row_time, on_row_time_clicked, LV_EVENT_CLICKED, NULL);

    /* 行 2: 亮度（带 value）*/
    int bright_idx = current_backlight_idx();
    lv_obj_t *row_bright = ui_list_row(card,
        ICON_BRIGHTNESS, "亮度", BACKLIGHT_LABELS[bright_idx],
        UI_C_WARN, &s_ui.bright_value);
    lv_obj_add_event_cb(row_bright, on_row_bright_clicked, LV_EVENT_CLICKED, NULL);

    /* 行 3: 关于（最后一行去底分隔线）*/
    lv_obj_t *row_about = ui_list_row(card,
        ICON_INFO, "关于", NULL,
        UI_C_TEXT_MUTED, NULL);
    lv_obj_set_style_border_width(row_about, 0, 0);
    lv_obj_add_event_cb(row_about, on_row_about_clicked, LV_EVENT_CLICKED, NULL);
}

/* ============================================================================
 * 生命周期
 * ========================================================================= */

static lv_obj_t *settings_home_create(void)
{
    ESP_LOGI(TAG, "Creating settings home");

    s_ui.screen = lv_obj_create(NULL);
    ui_screen_setup(s_ui.screen);

    lv_obj_add_event_cb(s_ui.screen, on_screen_pressed, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(s_ui.screen, on_screen_gesture, LV_EVENT_GESTURE, NULL);

    s_ui.statusbar = app_shell_attach_statusbar(s_ui.screen);
    create_title(s_ui.screen);
    create_list_card(s_ui.screen);

    return s_ui.screen;
}

static void settings_home_destroy(void)
{
    ESP_LOGI(TAG, "Destroying settings home");
    if (s_ui.screen) {
        lv_obj_del(s_ui.screen);
        s_ui.screen = NULL;
    }
    s_ui.statusbar    = NULL;
    s_ui.bright_value = NULL;
}

static const page_callbacks_t s_callbacks = {
    .create  = settings_home_create,
    .destroy = settings_home_destroy,
    .update  = NULL,
};

const page_callbacks_t *settings_home_get_callbacks(void)
{
    return &s_callbacks;
}
