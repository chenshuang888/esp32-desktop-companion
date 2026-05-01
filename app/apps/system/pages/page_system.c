#include "page_system.h"
#include "app_router.h"
#include "system_manager.h"
#include "system_service.h"

#include "esp_log.h"
#include "lvgl.h"
#include "app_fonts.h"
#include <stdio.h>

/* ============================================================================
 * 配色（沿用项目风格：深紫 + 青绿，进度超阈值用橙色告警）
 * ========================================================================= */

#define COLOR_BG         0x1E1B2E
#define COLOR_CARD       0x2D2640
#define COLOR_CARD_ALT   0x3A3354
#define COLOR_ACCENT     0x06B6D4
#define COLOR_TEXT       0xF1ECFF
#define COLOR_MUTED      0x9B94B5
#define COLOR_WARN       0xF97316  /* 橙：CPU/MEM/DISK > 80% 警告 */
#define COLOR_CHARGING   0x10B981  /* 绿：电池充电中 */

#define WARN_THRESHOLD 80  /* CPU/MEM/DISK 超过该值进度条变橙 */

static const char *TAG = "page_system";

/* ============================================================================
 * UI 元素
 * ========================================================================= */

typedef struct {
    lv_obj_t *bar;          /* lv_bar */
    lv_obj_t *value_lbl;    /* "68%" */
} progress_row_t;

typedef struct {
    lv_obj_t *screen;
    lv_obj_t *back_btn;

    progress_row_t cpu;
    progress_row_t mem;
    progress_row_t disk;
    progress_row_t bat;
    lv_obj_t *bat_icon_lbl;   /* ⚡ 充电图标 */

    lv_obj_t *uptime_lbl;
    lv_obj_t *net_lbl;
    lv_obj_t *temp_lbl;

    uint32_t last_epoch;      /* 去重：manager epoch 变化才刷 */

    lv_style_t style_card;
    lv_style_t style_topbtn;
    lv_style_t style_topbtn_pressed;
    lv_style_t style_row;
} page_system_ui_t;

static page_system_ui_t s_ui = {0};

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
    lv_style_set_pad_all(&s_ui.style_card, 10);
    lv_style_set_shadow_width(&s_ui.style_card, 0);

    lv_style_init(&s_ui.style_topbtn);
    lv_style_set_bg_opa(&s_ui.style_topbtn, LV_OPA_TRANSP);
    lv_style_set_border_width(&s_ui.style_topbtn, 0);
    lv_style_set_shadow_width(&s_ui.style_topbtn, 0);
    lv_style_set_text_color(&s_ui.style_topbtn, lv_color_hex(COLOR_ACCENT));
    lv_style_set_pad_all(&s_ui.style_topbtn, 4);

    lv_style_init(&s_ui.style_topbtn_pressed);
    lv_style_set_bg_color(&s_ui.style_topbtn_pressed, lv_color_hex(COLOR_ACCENT));
    lv_style_set_bg_opa(&s_ui.style_topbtn_pressed, LV_OPA_20);

    lv_style_init(&s_ui.style_row);
    lv_style_set_bg_opa(&s_ui.style_row, LV_OPA_TRANSP);
    lv_style_set_border_width(&s_ui.style_row, 0);
    lv_style_set_pad_all(&s_ui.style_row, 0);
}

/* ============================================================================
 * 布局
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

    lv_obj_t *title = lv_label_create(s_ui.screen);
    lv_label_set_text(title, "System");
    lv_obj_set_style_text_color(title, lv_color_hex(COLOR_TEXT), 0);
    lv_obj_set_style_text_font(title, APP_FONT_TITLE, 0);
    lv_obj_align(title, LV_ALIGN_TOP_RIGHT, -14, 15);
}

/**
 * 创建一个进度条行：左标签 + 右百分比 + 底下 lv_bar
 * row: 220 宽，40 高
 */
static void create_progress_row(lv_obj_t *parent,
                                const char *label_text,
                                progress_row_t *out)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_add_style(row, &s_ui.style_row, 0);
    lv_obj_set_size(row, lv_pct(100), 38);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *lbl = lv_label_create(row);
    lv_label_set_text(lbl, label_text);
    lv_obj_set_style_text_color(lbl, lv_color_hex(COLOR_MUTED), 0);
    lv_obj_set_style_text_font(lbl, APP_FONT_TEXT, 0);
    lv_obj_align(lbl, LV_ALIGN_TOP_LEFT, 0, 0);

    out->value_lbl = lv_label_create(row);
    lv_label_set_text(out->value_lbl, "--");
    lv_obj_set_style_text_color(out->value_lbl, lv_color_hex(COLOR_TEXT), 0);
    lv_obj_set_style_text_font(out->value_lbl, APP_FONT_TEXT, 0);
    lv_obj_align(out->value_lbl, LV_ALIGN_TOP_RIGHT, 0, 0);

    out->bar = lv_bar_create(row);
    lv_obj_set_size(out->bar, lv_pct(100), 6);
    lv_obj_align(out->bar, LV_ALIGN_BOTTOM_MID, 0, -4);
    lv_bar_set_range(out->bar, 0, 100);
    lv_bar_set_value(out->bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(out->bar, lv_color_hex(COLOR_CARD_ALT), 0);
    lv_obj_set_style_bg_opa(out->bar, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(out->bar, 3, 0);
    lv_obj_set_style_bg_color(out->bar, lv_color_hex(COLOR_ACCENT), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(out->bar, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(out->bar, 3, LV_PART_INDICATOR);
}

static void create_progress_card(void)
{
    lv_obj_t *card = lv_obj_create(s_ui.screen);
    lv_obj_remove_style_all(card);
    lv_obj_add_style(card, &s_ui.style_card, 0);
    lv_obj_set_size(card, 220, 180);
    lv_obj_align(card, LV_ALIGN_TOP_MID, 0, 50);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(card, 4, 0);

    create_progress_row(card, "CPU",  &s_ui.cpu);
    create_progress_row(card, "MEM",  &s_ui.mem);
    create_progress_row(card, "DISK", &s_ui.disk);
    create_progress_row(card, "BAT",  &s_ui.bat);

    /* 充电图标叠加到 BAT 行的 value 标签左边，仅在充电时可见 */
    s_ui.bat_icon_lbl = lv_label_create(lv_obj_get_parent(s_ui.bat.value_lbl));
    lv_label_set_text(s_ui.bat_icon_lbl, LV_SYMBOL_CHARGE);
    lv_obj_set_style_text_color(s_ui.bat_icon_lbl, lv_color_hex(COLOR_CHARGING), 0);
    lv_obj_set_style_text_font(s_ui.bat_icon_lbl, APP_FONT_TEXT, 0);
    lv_obj_align_to(s_ui.bat_icon_lbl, s_ui.bat.value_lbl, LV_ALIGN_OUT_LEFT_MID, -6, 0);
    lv_obj_add_flag(s_ui.bat_icon_lbl, LV_OBJ_FLAG_HIDDEN);
}

static void create_info_card(void)
{
    lv_obj_t *card = lv_obj_create(s_ui.screen);
    lv_obj_remove_style_all(card);
    lv_obj_add_style(card, &s_ui.style_card, 0);
    lv_obj_set_size(card, 220, 80);
    lv_obj_align(card, LV_ALIGN_TOP_MID, 0, 240);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(card, 0, 0);

    s_ui.uptime_lbl = lv_label_create(card);
    lv_label_set_text(s_ui.uptime_lbl, "Uptime  --");
    lv_obj_set_style_text_color(s_ui.uptime_lbl, lv_color_hex(COLOR_TEXT), 0);
    lv_obj_set_style_text_font(s_ui.uptime_lbl, APP_FONT_TEXT, 0);

    s_ui.net_lbl = lv_label_create(card);
    lv_label_set_text(s_ui.net_lbl, LV_SYMBOL_DOWN " --  " LV_SYMBOL_UP " --");
    lv_obj_set_style_text_color(s_ui.net_lbl, lv_color_hex(COLOR_TEXT), 0);
    lv_obj_set_style_text_font(s_ui.net_lbl, APP_FONT_TEXT, 0);

    s_ui.temp_lbl = lv_label_create(card);
    lv_label_set_text(s_ui.temp_lbl, "CPU Temp  --");
    lv_obj_set_style_text_color(s_ui.temp_lbl, lv_color_hex(COLOR_TEXT), 0);
    lv_obj_set_style_text_font(s_ui.temp_lbl, APP_FONT_TEXT, 0);
}

/* ============================================================================
 * 事件
 * ========================================================================= */

static void on_back_clicked(lv_event_t *e)
{
    app_router_exit_to_launcher();
}

static void bind_events(void)
{
    lv_obj_add_event_cb(s_ui.back_btn, on_back_clicked, LV_EVENT_CLICKED, NULL);
}

/* ============================================================================
 * 刷新
 * ========================================================================= */

static void apply_progress(progress_row_t *row, uint8_t value,
                           bool warn_on_high)
{
    if (value > 100) value = 100;
    lv_bar_set_value(row->bar, value, LV_ANIM_OFF);

    char buf[8];
    snprintf(buf, sizeof(buf), "%u%%", value);
    lv_label_set_text(row->value_lbl, buf);

    uint32_t color = COLOR_ACCENT;
    if (warn_on_high && value >= WARN_THRESHOLD) {
        color = COLOR_WARN;
    }
    lv_obj_set_style_bg_color(row->bar, lv_color_hex(color), LV_PART_INDICATOR);
    lv_obj_set_style_text_color(row->value_lbl, lv_color_hex(
        warn_on_high && value >= WARN_THRESHOLD ? COLOR_WARN : COLOR_TEXT), 0);
}

static void apply_battery(const system_payload_t *s)
{
    if (s->battery_percent == SYSTEM_BATTERY_ABSENT) {
        /* 无电池（台式机）：进度条置灰 + 文字 "N/A" + 隐藏充电图标 */
        lv_bar_set_value(s_ui.bat.bar, 0, LV_ANIM_OFF);
        lv_obj_set_style_bg_color(s_ui.bat.bar, lv_color_hex(COLOR_CARD_ALT), LV_PART_INDICATOR);
        lv_label_set_text(s_ui.bat.value_lbl, "N/A");
        lv_obj_set_style_text_color(s_ui.bat.value_lbl, lv_color_hex(COLOR_MUTED), 0);
        lv_obj_add_flag(s_ui.bat_icon_lbl, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    uint8_t v = s->battery_percent;
    if (v > 100) v = 100;
    lv_bar_set_value(s_ui.bat.bar, v, LV_ANIM_OFF);

    char buf[8];
    snprintf(buf, sizeof(buf), "%u%%", v);
    lv_label_set_text(s_ui.bat.value_lbl, buf);

    /* 电池低于 20 用橙色警告；充电中用绿色；其余青色 */
    bool charging = (s->battery_charging == 1);
    uint32_t color;
    if (charging) color = COLOR_CHARGING;
    else if (v <= 20) color = COLOR_WARN;
    else color = COLOR_ACCENT;
    lv_obj_set_style_bg_color(s_ui.bat.bar, lv_color_hex(color), LV_PART_INDICATOR);
    lv_obj_set_style_text_color(s_ui.bat.value_lbl, lv_color_hex(COLOR_TEXT), 0);

    if (charging) {
        lv_obj_clear_flag(s_ui.bat_icon_lbl, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_ui.bat_icon_lbl, LV_OBJ_FLAG_HIDDEN);
    }
}

static void apply_info(const system_payload_t *s)
{
    char buf[48];

    /* Uptime：按时长选合适单位 */
    uint32_t sec = s->uptime_sec;
    uint32_t d = sec / 86400;
    uint32_t h = (sec % 86400) / 3600;
    uint32_t m = (sec % 3600) / 60;
    if (d > 0) {
        snprintf(buf, sizeof(buf), "Uptime  %ud %uh %um", (unsigned)d, (unsigned)h, (unsigned)m);
    } else if (h > 0) {
        snprintf(buf, sizeof(buf), "Uptime  %uh %um", (unsigned)h, (unsigned)m);
    } else {
        snprintf(buf, sizeof(buf), "Uptime  %um", (unsigned)m);
    }
    lv_label_set_text(s_ui.uptime_lbl, buf);

    /* 网速：大于 1024 KB/s 显示 MB/s */
    char dn[16], up[16];
    if (s->net_down_kbps >= 1024) {
        snprintf(dn, sizeof(dn), "%u.%u MB/s",
                 s->net_down_kbps / 1024, (s->net_down_kbps % 1024) * 10 / 1024);
    } else {
        snprintf(dn, sizeof(dn), "%u KB/s", s->net_down_kbps);
    }
    if (s->net_up_kbps >= 1024) {
        snprintf(up, sizeof(up), "%u.%u MB/s",
                 s->net_up_kbps / 1024, (s->net_up_kbps % 1024) * 10 / 1024);
    } else {
        snprintf(up, sizeof(up), "%u KB/s", s->net_up_kbps);
    }
    snprintf(buf, sizeof(buf), LV_SYMBOL_DOWN " %s  " LV_SYMBOL_UP " %s", dn, up);
    lv_label_set_text(s_ui.net_lbl, buf);

    /* CPU 温度 */
    if (s->cpu_temp_cx10 == SYSTEM_CPU_TEMP_INVALID) {
        lv_label_set_text(s_ui.temp_lbl, "CPU Temp  --");
    } else {
        int t = s->cpu_temp_cx10;
        int ti = t / 10;
        int tf = (t < 0 ? -t : t) % 10;
        snprintf(buf, sizeof(buf), "CPU Temp  %d.%d°C", ti, tf);
        lv_label_set_text(s_ui.temp_lbl, buf);
    }
}

static void update_display(void)
{
    const system_payload_t *s = system_manager_get_latest();
    if (!s) {
        return;
    }

    uint32_t epoch = system_manager_get_epoch();
    if (epoch == s_ui.last_epoch) {
        return;
    }
    s_ui.last_epoch = epoch;

    apply_progress(&s_ui.cpu,  s->cpu_percent,  true);
    apply_progress(&s_ui.mem,  s->mem_percent,  true);
    apply_progress(&s_ui.disk, s->disk_percent, true);
    apply_battery(s);
    apply_info(s);
}

/* ============================================================================
 * 生命周期
 * ========================================================================= */

static void page_init(void)
{
    init_styles();
    create_top_bar();
    create_progress_card();
    create_info_card();
    bind_events();

    s_ui.last_epoch = 0;
    update_display();
}

static lv_obj_t *page_system_create(void)
{
    ESP_LOGI(TAG, "Creating system page");

    s_ui.screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_ui.screen, lv_color_hex(COLOR_BG), 0);

    page_init();

    /* 进入页面立刻让 PC 推一帧，不用等 2 秒 tick */
    system_service_send_request();

    return s_ui.screen;
}

static void page_system_destroy(void)
{
    ESP_LOGI(TAG, "Destroying system page");

    if (s_ui.screen) {
        lv_obj_del(s_ui.screen);
        s_ui.screen = NULL;
    }

    lv_style_reset(&s_ui.style_card);
    lv_style_reset(&s_ui.style_topbtn);
    lv_style_reset(&s_ui.style_topbtn_pressed);
    lv_style_reset(&s_ui.style_row);

    s_ui.back_btn = NULL;
    s_ui.cpu = (progress_row_t){0};
    s_ui.mem = (progress_row_t){0};
    s_ui.disk = (progress_row_t){0};
    s_ui.bat = (progress_row_t){0};
    s_ui.bat_icon_lbl = NULL;
    s_ui.uptime_lbl = NULL;
    s_ui.net_lbl = NULL;
    s_ui.temp_lbl = NULL;
}

static void page_system_update(void)
{
    update_display();
}

static const page_callbacks_t s_callbacks = {
    .create  = page_system_create,
    .destroy = page_system_destroy,
    .update  = page_system_update,
};

const page_callbacks_t *page_system_get_callbacks(void)
{
    return &s_callbacks;
}
