#include "settings_time.h"
#include "settings_app.h"
#include "esp_log.h"
#include "lvgl.h"
#include "app_fonts.h"
#include <time.h>
#include <sys/time.h>

/* ============================================================================
 * 时间调节子页（settings app）
 *
 * 入口：settings_home → 行点击 → settings_app_push(SETTINGS_PAGE_TIME)
 * 退出：底部 50px 边缘上滑 → settings_app_pop_or_exit()
 *
 * 主题：保留旧深紫主题（暂未换 token；P3 之外可单独迁移）
 * ========================================================================= */

#define COLOR_BG         0x1E1B2E
#define COLOR_CARD       0x2D2640
#define COLOR_CARD_ALT   0x3A3354
#define COLOR_ACCENT     0x06B6D4
#define COLOR_TEXT       0xF1ECFF
#define COLOR_MUTED      0x9B94B5

#define EXIT_EDGE_HEIGHT 50

static const char *TAG = "settings_time";

typedef struct {
    lv_obj_t *screen;

    lv_obj_t *preview_label;

    lv_obj_t *hour_up, *hour_dn;
    lv_obj_t *min_up, *min_dn;
    lv_obj_t *year_up, *year_dn;
    lv_obj_t *mon_up, *mon_dn;
    lv_obj_t *day_up, *day_dn;

    lv_style_t style_card;
    lv_style_t style_btn;
    lv_style_t style_btn_pressed;

    int press_y0;
} ui_t;

static ui_t s_ui = { .press_y0 = -1 };

/* ============================================================================
 * 样式
 * ========================================================================= */

static void init_styles(void)
{
    lv_style_init(&s_ui.style_card);
    lv_style_set_bg_color(&s_ui.style_card, lv_color_hex(COLOR_CARD));
    lv_style_set_bg_opa(&s_ui.style_card, LV_OPA_COVER);
    lv_style_set_radius(&s_ui.style_card, 12);
    lv_style_set_border_width(&s_ui.style_card, 0);
    lv_style_set_pad_all(&s_ui.style_card, 12);
    lv_style_set_shadow_width(&s_ui.style_card, 0);

    lv_style_init(&s_ui.style_btn);
    lv_style_set_bg_color(&s_ui.style_btn, lv_color_hex(COLOR_CARD_ALT));
    lv_style_set_bg_opa(&s_ui.style_btn, LV_OPA_COVER);
    lv_style_set_radius(&s_ui.style_btn, LV_RADIUS_CIRCLE);
    lv_style_set_border_width(&s_ui.style_btn, 0);
    lv_style_set_shadow_width(&s_ui.style_btn, 0);
    lv_style_set_text_color(&s_ui.style_btn, lv_color_hex(COLOR_ACCENT));
    lv_style_set_text_font(&s_ui.style_btn, APP_FONT_TITLE);
    lv_style_set_pad_all(&s_ui.style_btn, 0);

    lv_style_init(&s_ui.style_btn_pressed);
    lv_style_set_bg_color(&s_ui.style_btn_pressed, lv_color_hex(COLOR_ACCENT));
    lv_style_set_text_color(&s_ui.style_btn_pressed, lv_color_hex(COLOR_TEXT));
}

/* ============================================================================
 * 布局原语
 * ========================================================================= */

static lv_obj_t *create_card(lv_obj_t *parent, int y, int w, int h)
{
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_remove_style_all(card);
    lv_obj_add_style(card, &s_ui.style_card, 0);
    lv_obj_set_size(card, w, h);
    lv_obj_align(card, LV_ALIGN_TOP_MID, 0, y);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    return card;
}

static lv_obj_t *create_round_btn(lv_obj_t *parent, const char *symbol, int size)
{
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_remove_style_all(btn);
    lv_obj_add_style(btn, &s_ui.style_btn, 0);
    lv_obj_add_style(btn, &s_ui.style_btn_pressed, LV_STATE_PRESSED);
    lv_obj_set_size(btn, size, size);

    lv_obj_t *label = lv_label_create(btn);
    lv_label_set_text(label, symbol);
    lv_obj_center(label);
    return btn;
}

static lv_obj_t *create_small_label(lv_obj_t *parent, const char *text, uint32_t color)
{
    lv_obj_t *lbl = lv_label_create(parent);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_color(lbl, lv_color_hex(color), 0);
    lv_obj_set_style_text_font(lbl, APP_FONT_TEXT, 0);
    return lbl;
}

static void create_preview(void)
{
    s_ui.preview_label = lv_label_create(s_ui.screen);
    lv_label_set_text(s_ui.preview_label, "--:--:--");
    lv_obj_set_style_text_color(s_ui.preview_label, lv_color_hex(COLOR_ACCENT), 0);
    lv_obj_set_style_text_font(s_ui.preview_label, APP_FONT_LARGE, 0);
    lv_obj_align(s_ui.preview_label, LV_ALIGN_TOP_MID, 0, 18);
}

static void create_time_adjust_card(void)
{
    lv_obj_t *card = create_card(s_ui.screen, 55, 220, 100);

    lv_obj_t *title = create_small_label(card, "TIME", COLOR_MUTED);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 0, 0);

    const int btn = 34;
    const int group_w = btn * 2 + 2;
    const int gap = 196 - group_w * 2;

    lv_obj_t *h_lbl = create_small_label(card, "Hour", COLOR_TEXT);
    lv_obj_align(h_lbl, LV_ALIGN_TOP_LEFT, group_w / 2 - 14, 22);

    s_ui.hour_dn = create_round_btn(card, "-", btn);
    lv_obj_align(s_ui.hour_dn, LV_ALIGN_TOP_LEFT, 0, 44);

    s_ui.hour_up = create_round_btn(card, "+", btn);
    lv_obj_align(s_ui.hour_up, LV_ALIGN_TOP_LEFT, btn + 2, 44);

    int min_x = group_w + gap;
    lv_obj_t *m_lbl = create_small_label(card, "Min", COLOR_TEXT);
    lv_obj_align(m_lbl, LV_ALIGN_TOP_LEFT, min_x + group_w / 2 - 11, 22);

    s_ui.min_dn = create_round_btn(card, "-", btn);
    lv_obj_align(s_ui.min_dn, LV_ALIGN_TOP_LEFT, min_x, 44);

    s_ui.min_up = create_round_btn(card, "+", btn);
    lv_obj_align(s_ui.min_up, LV_ALIGN_TOP_LEFT, min_x + btn + 2, 44);
}

static void create_date_adjust_card(void)
{
    lv_obj_t *card = create_card(s_ui.screen, 170, 220, 140);

    lv_obj_t *title = create_small_label(card, "DATE", COLOR_MUTED);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 0, 0);

    const int btn = 32;
    const int group_w = btn * 2 + 2;
    const int gap = (196 - group_w * 3) / 2;

    struct {
        const char *label;
        int label_half_w;
        lv_obj_t **dn;
        lv_obj_t **up;
    } groups[] = {
        {"Year",  14, &s_ui.year_dn, &s_ui.year_up},
        {"Month", 17, &s_ui.mon_dn,  &s_ui.mon_up},
        {"Day",   11, &s_ui.day_dn,  &s_ui.day_up},
    };

    for (int i = 0; i < 3; i++) {
        int x = i * (group_w + gap);
        lv_obj_t *lbl = create_small_label(card, groups[i].label, COLOR_TEXT);
        lv_obj_align(lbl, LV_ALIGN_TOP_LEFT, x + group_w / 2 - groups[i].label_half_w, 22);

        *groups[i].dn = create_round_btn(card, "-", btn);
        lv_obj_align(*groups[i].dn, LV_ALIGN_TOP_LEFT, x, 48);

        *groups[i].up = create_round_btn(card, "+", btn);
        lv_obj_align(*groups[i].up, LV_ALIGN_TOP_LEFT, x + btn + 2, 48);
    }
}

/* ============================================================================
 * 业务
 * ========================================================================= */

static void adjust_time(int hour_d, int min_d, int sec_d)
{
    time_t now;
    struct tm t;
    time(&now); localtime_r(&now, &t);
    t.tm_hour += hour_d; t.tm_min += min_d; t.tm_sec += sec_d;
    time_t new_t = mktime(&t);
    struct timeval tv = { .tv_sec = new_t, .tv_usec = 0 };
    settimeofday(&tv, NULL);
}

static void adjust_date(int year_d, int mon_d, int day_d)
{
    time_t now;
    struct tm t;
    time(&now); localtime_r(&now, &t);
    t.tm_year += year_d; t.tm_mon += mon_d; t.tm_mday += day_d;
    time_t new_t = mktime(&t);
    struct timeval tv = { .tv_sec = new_t, .tv_usec = 0 };
    settimeofday(&tv, NULL);
}

static void on_hour_up(lv_event_t *e)  { (void)e; adjust_time(1, 0, 0); }
static void on_hour_dn(lv_event_t *e)  { (void)e; adjust_time(-1, 0, 0); }
static void on_min_up(lv_event_t *e)   { (void)e; adjust_time(0, 1, 0); }
static void on_min_dn(lv_event_t *e)   { (void)e; adjust_time(0, -1, 0); }
static void on_year_up(lv_event_t *e)  { (void)e; adjust_date(1, 0, 0); }
static void on_year_dn(lv_event_t *e)  { (void)e; adjust_date(-1, 0, 0); }
static void on_mon_up(lv_event_t *e)   { (void)e; adjust_date(0, 1, 0); }
static void on_mon_dn(lv_event_t *e)   { (void)e; adjust_date(0, -1, 0); }
static void on_day_up(lv_event_t *e)   { (void)e; adjust_date(0, 0, 1); }
static void on_day_dn(lv_event_t *e)   { (void)e; adjust_date(0, 0, -1); }

/* ---- 底缘上滑 pop ---- */

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

static void bind_events(void)
{
    lv_obj_add_event_cb(s_ui.hour_up,  on_hour_up,  LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(s_ui.hour_dn,  on_hour_dn,  LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(s_ui.min_up,   on_min_up,   LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(s_ui.min_dn,   on_min_dn,   LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(s_ui.year_up,  on_year_up,  LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(s_ui.year_dn,  on_year_dn,  LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(s_ui.mon_up,   on_mon_up,   LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(s_ui.mon_dn,   on_mon_dn,   LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(s_ui.day_up,   on_day_up,   LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(s_ui.day_dn,   on_day_dn,   LV_EVENT_CLICKED, NULL);

    lv_obj_add_event_cb(s_ui.screen,   on_pressed,  LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(s_ui.screen,   on_gesture,  LV_EVENT_GESTURE, NULL);
}

static void update_display(void)
{
    if (!s_ui.preview_label) return;
    time_t now; struct tm t;
    time(&now); localtime_r(&now, &t);
    char buf[12];
    snprintf(buf, sizeof(buf), "%02d:%02d:%02d", t.tm_hour, t.tm_min, t.tm_sec);
    lv_label_set_text(s_ui.preview_label, buf);
}

/* ============================================================================
 * 生命周期
 * ========================================================================= */

static lv_obj_t *create(void)
{
    ESP_LOGI(TAG, "create");

    s_ui.screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_ui.screen, lv_color_hex(COLOR_BG), 0);
    s_ui.press_y0 = -1;

    init_styles();
    create_preview();
    create_time_adjust_card();
    create_date_adjust_card();
    bind_events();
    update_display();
    return s_ui.screen;
}

static void destroy(void)
{
    ESP_LOGI(TAG, "destroy");
    if (s_ui.screen) {
        lv_obj_del(s_ui.screen);
        s_ui.screen = NULL;
    }
    lv_style_reset(&s_ui.style_card);
    lv_style_reset(&s_ui.style_btn);
    lv_style_reset(&s_ui.style_btn_pressed);
    s_ui.preview_label = NULL;
    s_ui.hour_up = s_ui.hour_dn = NULL;
    s_ui.min_up  = s_ui.min_dn  = NULL;
    s_ui.year_up = s_ui.year_dn = NULL;
    s_ui.mon_up  = s_ui.mon_dn  = NULL;
    s_ui.day_up  = s_ui.day_dn  = NULL;
}

static void update_cb(void) { update_display(); }

static const page_callbacks_t s_callbacks = {
    .create  = create,
    .destroy = destroy,
    .update  = update_cb,
};

const page_callbacks_t *settings_time_get_callbacks(void)
{
    return &s_callbacks;
}
