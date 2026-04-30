#include "page_time_adjust.h"
#include "esp_log.h"
#include "lvgl.h"
#include "app_fonts.h"
#include <time.h>
#include <sys/time.h>

/* ============================================================================
 * 配色（与时间/菜单页一致）
 * ========================================================================= */

#define COLOR_BG         0x1E1B2E
#define COLOR_CARD       0x2D2640
#define COLOR_CARD_ALT   0x3A3354
#define COLOR_ACCENT     0x06B6D4
#define COLOR_TEXT       0xF1ECFF
#define COLOR_MUTED      0x9B94B5

static const char *TAG = "page_time_adjust";

/* ============================================================================
 * UI 元素
 * ========================================================================= */

typedef struct {
    lv_obj_t *screen;
    lv_obj_t *back_btn;

    lv_obj_t *preview_label;   /* 顶部实时时间回显，调节按钮按下后立刻反映变化 */

    lv_obj_t *hour_up, *hour_dn;
    lv_obj_t *min_up, *min_dn;
    lv_obj_t *year_up, *year_dn;
    lv_obj_t *mon_up, *mon_dn;
    lv_obj_t *day_up, *day_dn;

    lv_style_t style_card;
    lv_style_t style_btn;
    lv_style_t style_btn_pressed;
    lv_style_t style_topbtn;
    lv_style_t style_topbtn_pressed;
} page_time_adjust_ui_t;

static page_time_adjust_ui_t s_ui = {0};

/* ============================================================================
 * CSS - 样式
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

    /* 顶部返回按钮（复用 page_about / page_menu 的模板） */
    lv_style_init(&s_ui.style_topbtn);
    lv_style_set_bg_opa(&s_ui.style_topbtn, LV_OPA_TRANSP);
    lv_style_set_border_width(&s_ui.style_topbtn, 0);
    lv_style_set_shadow_width(&s_ui.style_topbtn, 0);
    lv_style_set_text_color(&s_ui.style_topbtn, lv_color_hex(COLOR_ACCENT));
    lv_style_set_pad_all(&s_ui.style_topbtn, 4);

    lv_style_init(&s_ui.style_topbtn_pressed);
    lv_style_set_bg_color(&s_ui.style_topbtn_pressed, lv_color_hex(COLOR_ACCENT));
    lv_style_set_bg_opa(&s_ui.style_topbtn_pressed, LV_OPA_20);
}

/* ============================================================================
 * HTML - 布局
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

static void create_top_bar(void)
{
    s_ui.back_btn = lv_btn_create(s_ui.screen);
    lv_obj_remove_style_all(s_ui.back_btn);
    lv_obj_add_style(s_ui.back_btn, &s_ui.style_topbtn, 0);
    lv_obj_add_style(s_ui.back_btn, &s_ui.style_topbtn_pressed, LV_STATE_PRESSED);
    lv_obj_set_style_radius(s_ui.back_btn, 6, 0);
    lv_obj_set_size(s_ui.back_btn, 80, 30);
    lv_obj_align(s_ui.back_btn, LV_ALIGN_TOP_LEFT, 10, 10);

    lv_obj_t *lbl = lv_label_create(s_ui.back_btn);
    lv_label_set_text(lbl, LV_SYMBOL_LEFT " Time");
    lv_obj_set_style_text_font(lbl, APP_FONT_TEXT, 0);
    lv_obj_center(lbl);
}

static void create_preview(void)
{
    /* 顶部 Back 右侧的实时时间回显，调节时能立刻看到变化 */
    s_ui.preview_label = lv_label_create(s_ui.screen);
    lv_label_set_text(s_ui.preview_label, "--:--:--");
    lv_obj_set_style_text_color(s_ui.preview_label, lv_color_hex(COLOR_ACCENT), 0);
    lv_obj_set_style_text_font(s_ui.preview_label, APP_FONT_LARGE, 0);
    lv_obj_align(s_ui.preview_label, LV_ALIGN_TOP_RIGHT, -14, 12);
}

static void create_time_adjust_card(void)
{
    lv_obj_t *card = create_card(s_ui.screen, 55, 220, 100);

    lv_obj_t *title = create_small_label(card, "TIME", COLOR_MUTED);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 0, 0);

    const int btn = 34;
    const int group_w = btn * 2 + 2;
    const int gap = 196 - group_w * 2;

    /* Hour group */
    lv_obj_t *h_lbl = create_small_label(card, "Hour", COLOR_TEXT);
    lv_obj_align(h_lbl, LV_ALIGN_TOP_LEFT, group_w / 2 - 14, 22);

    s_ui.hour_dn = create_round_btn(card, "-", btn);
    lv_obj_align(s_ui.hour_dn, LV_ALIGN_TOP_LEFT, 0, 44);

    s_ui.hour_up = create_round_btn(card, "+", btn);
    lv_obj_align(s_ui.hour_up, LV_ALIGN_TOP_LEFT, btn + 2, 44);

    /* Min group */
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

    /* 三组: 按钮 32, 组宽 66, 横排 */
    const int btn = 32;
    const int group_w = btn * 2 + 2;                 /* 66 */
    const int gap = (196 - group_w * 3) / 2;         /* 相邻两组间距 */

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
 * 业务逻辑 - 时间调整（保留原 page_time 的直接 settimeofday 写法；
 * settings_store_tick_save_time 会在 UI 主循环里把新时间落盘到 NVS）
 * ========================================================================= */

static void adjust_time(int hour_d, int min_d, int sec_d)
{
    time_t now;
    struct tm timeinfo;
    time(&now);
    localtime_r(&now, &timeinfo);

    timeinfo.tm_hour += hour_d;
    timeinfo.tm_min  += min_d;
    timeinfo.tm_sec  += sec_d;

    time_t t = mktime(&timeinfo);
    struct timeval tv = { .tv_sec = t, .tv_usec = 0 };
    settimeofday(&tv, NULL);
}

static void adjust_date(int year_d, int mon_d, int day_d)
{
    time_t now;
    struct tm timeinfo;
    time(&now);
    localtime_r(&now, &timeinfo);

    timeinfo.tm_year += year_d;
    timeinfo.tm_mon  += mon_d;
    timeinfo.tm_mday += day_d;

    time_t t = mktime(&timeinfo);
    struct timeval tv = { .tv_sec = t, .tv_usec = 0 };
    settimeofday(&tv, NULL);
}

/* ============================================================================
 * 事件回调
 * ========================================================================= */

static void on_hour_up(lv_event_t *e)  { adjust_time(1, 0, 0); }
static void on_hour_dn(lv_event_t *e)  { adjust_time(-1, 0, 0); }
static void on_min_up(lv_event_t *e)   { adjust_time(0, 1, 0); }
static void on_min_dn(lv_event_t *e)   { adjust_time(0, -1, 0); }
static void on_year_up(lv_event_t *e)  { adjust_date(1, 0, 0); }
static void on_year_dn(lv_event_t *e)  { adjust_date(-1, 0, 0); }
static void on_mon_up(lv_event_t *e)   { adjust_date(0, 1, 0); }
static void on_mon_dn(lv_event_t *e)   { adjust_date(0, -1, 0); }
static void on_day_up(lv_event_t *e)   { adjust_date(0, 0, 1); }
static void on_day_dn(lv_event_t *e)   { adjust_date(0, 0, -1); }

static void on_back_clicked(lv_event_t *e)
{
    page_router_switch(PAGE_SETTINGS);
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
    lv_obj_add_event_cb(s_ui.back_btn, on_back_clicked, LV_EVENT_CLICKED, NULL);
}

/* ============================================================================
 * 显示刷新
 * ========================================================================= */

static void update_display(void)
{
    if (!s_ui.preview_label) return;

    time_t now;
    struct tm t;
    time(&now);
    localtime_r(&now, &t);

    char buf[12];
    snprintf(buf, sizeof(buf), "%02d:%02d:%02d",
             t.tm_hour, t.tm_min, t.tm_sec);
    lv_label_set_text(s_ui.preview_label, buf);
}

/* ============================================================================
 * 页面生命周期
 * ========================================================================= */

static lv_obj_t *page_time_adjust_create(void)
{
    ESP_LOGI(TAG, "Creating time adjust page");

    s_ui.screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_ui.screen, lv_color_hex(COLOR_BG), 0);

    init_styles();
    create_top_bar();
    create_preview();
    create_time_adjust_card();
    create_date_adjust_card();
    bind_events();

    update_display();
    return s_ui.screen;
}

static void page_time_adjust_destroy(void)
{
    ESP_LOGI(TAG, "Destroying time adjust page");

    if (s_ui.screen) {
        lv_obj_del(s_ui.screen);
        s_ui.screen = NULL;
    }

    lv_style_reset(&s_ui.style_card);
    lv_style_reset(&s_ui.style_btn);
    lv_style_reset(&s_ui.style_btn_pressed);
    lv_style_reset(&s_ui.style_topbtn);
    lv_style_reset(&s_ui.style_topbtn_pressed);

    s_ui.back_btn = NULL;
    s_ui.preview_label = NULL;
    s_ui.hour_up = s_ui.hour_dn = NULL;
    s_ui.min_up  = s_ui.min_dn  = NULL;
    s_ui.year_up = s_ui.year_dn = NULL;
    s_ui.mon_up  = s_ui.mon_dn  = NULL;
    s_ui.day_up  = s_ui.day_dn  = NULL;
}

static void page_time_adjust_update(void)
{
    update_display();
}

static const page_callbacks_t s_callbacks = {
    .create  = page_time_adjust_create,
    .destroy = page_time_adjust_destroy,
    .update  = page_time_adjust_update,
};

const page_callbacks_t *page_time_adjust_get_callbacks(void)
{
    return &s_callbacks;
}
