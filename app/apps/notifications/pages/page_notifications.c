#include "page_notifications.h"
#include "app_router.h"
#include "notify_manager.h"

#include "esp_log.h"
#include "lvgl.h"
#include "app_fonts.h"
#include <stdio.h>
#include <time.h>

/* ============================================================================
 * 配色（沿用项目风格）
 * ========================================================================= */

#define COLOR_BG         0x1E1B2E
#define COLOR_CARD       0x2D2640
#define COLOR_CARD_ALT   0x3A3354
#define COLOR_ACCENT     0x06B6D4
#define COLOR_TEXT       0xF1ECFF
#define COLOR_MUTED      0x9B94B5

static const char *TAG = "page_notify";

/* ============================================================================
 * UI 元素
 * ========================================================================= */

typedef struct {
    lv_obj_t *screen;
    lv_obj_t *back_btn;
    lv_obj_t *count_lbl;   /* 右上 "3/10" 小字 */
    lv_obj_t *list;        /* 可滚动列表容器 */
    lv_obj_t *empty_lbl;   /* 空状态提示 */

    uint32_t last_version;

    lv_style_t style_topbtn;
    lv_style_t style_topbtn_pressed;
    lv_style_t style_item_card;
} page_notify_ui_t;

static page_notify_ui_t s_ui = {0};

/* ============================================================================
 * category → (symbol, color) 映射
 * ========================================================================= */

typedef struct {
    const char *symbol;
    uint32_t    color;
} category_style_t;

static const category_style_t CAT_STYLES[] = {
    [NOTIFY_CAT_GENERIC]  = { LV_SYMBOL_BELL,      COLOR_MUTED },
    [NOTIFY_CAT_MESSAGE]  = { LV_SYMBOL_ENVELOPE,  COLOR_ACCENT },
    [NOTIFY_CAT_EMAIL]    = { LV_SYMBOL_ENVELOPE,  0xFBBF24 },
    [NOTIFY_CAT_CALL]     = { LV_SYMBOL_CALL,      0x10B981 },
    [NOTIFY_CAT_CALENDAR] = { LV_SYMBOL_LIST,      0xA78BFA },
    [NOTIFY_CAT_SOCIAL]   = { LV_SYMBOL_EYE_OPEN,  0x06B6D4 },
    [NOTIFY_CAT_NEWS]     = { LV_SYMBOL_FILE,      COLOR_MUTED },
    [NOTIFY_CAT_ALERT]    = { LV_SYMBOL_WARNING,   0xF97316 },
};

static const category_style_t *get_cat_style(uint8_t cat)
{
    if (cat >= sizeof(CAT_STYLES) / sizeof(CAT_STYLES[0])) {
        return &CAT_STYLES[NOTIFY_CAT_GENERIC];
    }
    return &CAT_STYLES[cat];
}

/* ============================================================================
 * CSS - 样式
 * ========================================================================= */

static void init_styles(void)
{
    lv_style_init(&s_ui.style_topbtn);
    lv_style_set_bg_opa(&s_ui.style_topbtn, LV_OPA_TRANSP);
    lv_style_set_border_width(&s_ui.style_topbtn, 0);
    lv_style_set_shadow_width(&s_ui.style_topbtn, 0);
    lv_style_set_text_color(&s_ui.style_topbtn, lv_color_hex(COLOR_ACCENT));
    lv_style_set_pad_all(&s_ui.style_topbtn, 4);

    lv_style_init(&s_ui.style_topbtn_pressed);
    lv_style_set_bg_color(&s_ui.style_topbtn_pressed, lv_color_hex(COLOR_ACCENT));
    lv_style_set_bg_opa(&s_ui.style_topbtn_pressed, LV_OPA_20);

    lv_style_init(&s_ui.style_item_card);
    lv_style_set_bg_color(&s_ui.style_item_card, lv_color_hex(COLOR_CARD));
    lv_style_set_bg_opa(&s_ui.style_item_card, LV_OPA_COVER);
    lv_style_set_radius(&s_ui.style_item_card, 10);
    lv_style_set_border_width(&s_ui.style_item_card, 0);
    lv_style_set_pad_all(&s_ui.style_item_card, 8);
    lv_style_set_shadow_width(&s_ui.style_item_card, 0);
}

/* ============================================================================
 * HTML - 布局
 * ========================================================================= */

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
    lv_label_set_text(lbl, LV_SYMBOL_LEFT " Back");
    lv_obj_set_style_text_font(lbl, APP_FONT_TEXT, 0);
    lv_obj_center(lbl);

    s_ui.count_lbl = lv_label_create(s_ui.screen);
    lv_label_set_text(s_ui.count_lbl, "0/10");
    lv_obj_set_style_text_color(s_ui.count_lbl, lv_color_hex(COLOR_MUTED), 0);
    lv_obj_set_style_text_font(s_ui.count_lbl, APP_FONT_TEXT, 0);
    lv_obj_align(s_ui.count_lbl, LV_ALIGN_TOP_RIGHT, -14, 18);
}

static void create_list_container(void)
{
    s_ui.list = lv_obj_create(s_ui.screen);
    lv_obj_remove_style_all(s_ui.list);
    lv_obj_set_size(s_ui.list, 220, 262);
    lv_obj_align(s_ui.list, LV_ALIGN_TOP_MID, 0, 48);

    /* 纵向 flex，可滚动 */
    lv_obj_set_flex_flow(s_ui.list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_ui.list, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(s_ui.list, 6, 0);
    lv_obj_set_style_pad_all(s_ui.list, 2, 0);
    /* 默认即可滚动，无需 clear SCROLLABLE flag */
    lv_obj_set_scroll_dir(s_ui.list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_ui.list, LV_SCROLLBAR_MODE_AUTO);
}

/* ============================================================================
 * 单条通知卡片
 *
 *  [icon]  Title (ellipsis)              HH:MM
 *          Body line 1...
 *          Body line 2... (wrap)
 * ========================================================================= */

static void build_notification_item(lv_obj_t *parent,
                                    const notification_payload_t *n)
{
    const category_style_t *cs = get_cat_style(n->category);

    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_remove_style_all(card);
    lv_obj_add_style(card, &s_ui.style_item_card, 0);
    lv_obj_set_size(card, lv_pct(100), 76);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    /* 左上图标 */
    lv_obj_t *icon = lv_label_create(card);
    lv_label_set_text(icon, cs->symbol);
    lv_obj_set_style_text_color(icon, lv_color_hex(cs->color), 0);
    lv_obj_set_style_text_font(icon, APP_FONT_TITLE, 0);
    lv_obj_align(icon, LV_ALIGN_TOP_LEFT, 0, 0);

    /* 时间 HH:MM（右上） */
    char time_buf[8];
    time_t ts = (time_t)n->timestamp;
    struct tm tm;
    localtime_r(&ts, &tm);
    snprintf(time_buf, sizeof(time_buf), "%02d:%02d", tm.tm_hour, tm.tm_min);

    lv_obj_t *time_lbl = lv_label_create(card);
    lv_label_set_text(time_lbl, time_buf);
    lv_obj_set_style_text_color(time_lbl, lv_color_hex(COLOR_MUTED), 0);
    lv_obj_set_style_text_font(time_lbl, APP_FONT_TEXT, 0);
    lv_obj_align(time_lbl, LV_ALIGN_TOP_RIGHT, 0, 2);

    /* 标题：单行省略 */
    lv_obj_t *title_lbl = lv_label_create(card);
    lv_label_set_text(title_lbl, n->title[0] ? n->title : "(no title)");
    lv_obj_set_style_text_color(title_lbl, lv_color_hex(COLOR_TEXT), 0);
    lv_obj_set_style_text_font(title_lbl, APP_FONT_TEXT, 0);
    lv_label_set_long_mode(title_lbl, LV_LABEL_LONG_DOT);
    lv_obj_set_width(title_lbl, 110);
    lv_obj_align(title_lbl, LV_ALIGN_TOP_LEFT, 32, 2);

    /* 正文：两行 wrap + 省略 */
    lv_obj_t *body_lbl = lv_label_create(card);
    lv_label_set_text(body_lbl, n->body[0] ? n->body : "");
    lv_obj_set_style_text_color(body_lbl, lv_color_hex(COLOR_MUTED), 0);
    lv_obj_set_style_text_font(body_lbl, APP_FONT_TEXT, 0);
    lv_label_set_long_mode(body_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(body_lbl, 184);
    lv_obj_set_height(body_lbl, 36);
    lv_obj_align(body_lbl, LV_ALIGN_TOP_LEFT, 0, 26);
}

/* ============================================================================
 * 事件
 * ========================================================================= */

static void on_back_clicked(lv_event_t *e)
{
    app_router_exit_to_launcher();
}

/* ============================================================================
 * 刷新
 * ========================================================================= */

static void refresh_list(void)
{
    /* 清空现有条目 */
    lv_obj_clean(s_ui.list);

    size_t n = notify_manager_count();

    char cnt_buf[12];
    snprintf(cnt_buf, sizeof(cnt_buf), "%u/%d",
             (unsigned)n, (int)NOTIFY_STORE_MAX);
    lv_label_set_text(s_ui.count_lbl, cnt_buf);

    if (n == 0) {
        if (!s_ui.empty_lbl) {
            s_ui.empty_lbl = lv_label_create(s_ui.screen);
            lv_label_set_text(s_ui.empty_lbl, "No notifications");
            lv_obj_set_style_text_color(s_ui.empty_lbl, lv_color_hex(COLOR_MUTED), 0);
            lv_obj_set_style_text_font(s_ui.empty_lbl, APP_FONT_TITLE, 0);
            lv_obj_center(s_ui.empty_lbl);
        }
        lv_obj_clear_flag(s_ui.empty_lbl, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    if (s_ui.empty_lbl) {
        lv_obj_add_flag(s_ui.empty_lbl, LV_OBJ_FLAG_HIDDEN);
    }

    for (size_t i = 0; i < n; i++) {
        const notification_payload_t *item = notify_manager_get_at(i);
        if (!item) continue;
        build_notification_item(s_ui.list, item);
    }
}

static void update_display(void)
{
    uint32_t v = notify_manager_version();
    if (v == s_ui.last_version) {
        return;
    }
    s_ui.last_version = v;
    refresh_list();
}

/* ============================================================================
 * 页面生命周期
 * ========================================================================= */

static void page_init(void)
{
    init_styles();
    create_top_bar();
    create_list_container();

    s_ui.last_version = (uint32_t)-1;  /* 强制首次刷新 */
    update_display();

    lv_obj_add_event_cb(s_ui.back_btn, on_back_clicked, LV_EVENT_CLICKED, NULL);
}

static lv_obj_t *page_notifications_create(void)
{
    ESP_LOGI(TAG, "Creating notifications page");

    s_ui.screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_ui.screen, lv_color_hex(COLOR_BG), 0);

    page_init();
    return s_ui.screen;
}

static void page_notifications_destroy(void)
{
    ESP_LOGI(TAG, "Destroying notifications page");

    if (s_ui.screen) {
        lv_obj_del(s_ui.screen);
        s_ui.screen = NULL;
    }

    lv_style_reset(&s_ui.style_topbtn);
    lv_style_reset(&s_ui.style_topbtn_pressed);
    lv_style_reset(&s_ui.style_item_card);

    s_ui.back_btn = NULL;
    s_ui.count_lbl = NULL;
    s_ui.list = NULL;
    s_ui.empty_lbl = NULL;
}

static void page_notifications_update(void)
{
    update_display();
}

static const page_callbacks_t s_callbacks = {
    .create  = page_notifications_create,
    .destroy = page_notifications_destroy,
    .update  = page_notifications_update,
};

const page_callbacks_t *page_notifications_get_callbacks(void)
{
    return &s_callbacks;
}
