#include "page_music.h"
#include "app_router.h"
#include "esp_log.h"
#include "lvgl.h"
#include "app_fonts.h"
#include "media_manager.h"
#include "media_service.h"
#include "ble_driver.h"
#include <stdio.h>
#include <string.h>

/* ============================================================================
 * 配色（沿用项目风格）
 * ========================================================================= */

#define COLOR_BG         0x1E1B2E
#define COLOR_CARD       0x2D2640
#define COLOR_CARD_ALT   0x3A3354
#define COLOR_ACCENT     0x06B6D4
#define COLOR_TEXT       0xF1ECFF
#define COLOR_MUTED      0x9B94B5
#define COLOR_SUCCESS    0x10B981
#define COLOR_OFFLINE    0x6B7280

static const char *TAG = "page_music";

/* ============================================================================
 * 媒体键 id（直接复用 media_service.h 的 MEDIA_BTN_* 宏，不再本地别名）
 * ========================================================================= */

/* ============================================================================
 * UI 元素
 * ========================================================================= */

typedef struct {
    lv_obj_t *screen;
    lv_obj_t *back_btn;
    lv_obj_t *status_lbl;       /* 顶栏右上角连接状态 */

    lv_obj_t *title_lbl;
    lv_obj_t *artist_lbl;
    lv_obj_t *slider;
    lv_obj_t *time_lbl;

    lv_obj_t *prev_btn;
    lv_obj_t *pp_btn;
    lv_obj_t *pp_icon;          /* ⏸/▶ 图标 label，单独引用便于切换 */
    lv_obj_t *next_btn;

    uint32_t  last_version;     /* 仅在 version 变化时重绘 title/artist/总时长/图标 */
    int16_t   last_pos_sec;     /* 进度条增量更新，避免每帧无谓刷新 */
    bool      last_connected;

    lv_style_t style_topbtn;
    lv_style_t style_topbtn_pressed;
    lv_style_t style_ctrl_btn;
    lv_style_t style_ctrl_btn_pressed;
    lv_style_t style_ctrl_btn_disabled;
} page_music_ui_t;

static page_music_ui_t s_ui = {0};

/* ============================================================================
 * CSS - 样式
 * ========================================================================= */

static void init_styles(void)
{
    /* 顶部返回按钮 */
    lv_style_init(&s_ui.style_topbtn);
    lv_style_set_bg_opa(&s_ui.style_topbtn, LV_OPA_TRANSP);
    lv_style_set_border_width(&s_ui.style_topbtn, 0);
    lv_style_set_shadow_width(&s_ui.style_topbtn, 0);
    lv_style_set_text_color(&s_ui.style_topbtn, lv_color_hex(COLOR_ACCENT));
    lv_style_set_pad_all(&s_ui.style_topbtn, 4);

    lv_style_init(&s_ui.style_topbtn_pressed);
    lv_style_set_bg_color(&s_ui.style_topbtn_pressed, lv_color_hex(COLOR_ACCENT));
    lv_style_set_bg_opa(&s_ui.style_topbtn_pressed, LV_OPA_20);

    /* 三个控制按钮 */
    lv_style_init(&s_ui.style_ctrl_btn);
    lv_style_set_bg_color(&s_ui.style_ctrl_btn, lv_color_hex(COLOR_CARD));
    lv_style_set_bg_opa(&s_ui.style_ctrl_btn, LV_OPA_COVER);
    lv_style_set_radius(&s_ui.style_ctrl_btn, LV_RADIUS_CIRCLE);
    lv_style_set_border_width(&s_ui.style_ctrl_btn, 2);
    lv_style_set_border_color(&s_ui.style_ctrl_btn, lv_color_hex(COLOR_CARD_ALT));
    lv_style_set_shadow_width(&s_ui.style_ctrl_btn, 0);
    lv_style_set_pad_all(&s_ui.style_ctrl_btn, 0);

    lv_style_init(&s_ui.style_ctrl_btn_pressed);
    lv_style_set_bg_color(&s_ui.style_ctrl_btn_pressed, lv_color_hex(COLOR_ACCENT));
    lv_style_set_bg_opa(&s_ui.style_ctrl_btn_pressed, LV_OPA_40);

    lv_style_init(&s_ui.style_ctrl_btn_disabled);
    lv_style_set_border_color(&s_ui.style_ctrl_btn_disabled, lv_color_hex(COLOR_OFFLINE));
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

    /* 顶栏右上角连接状态 */
    s_ui.status_lbl = lv_label_create(s_ui.screen);
    lv_label_set_text(s_ui.status_lbl, "Off");
    lv_obj_set_style_text_color(s_ui.status_lbl, lv_color_hex(COLOR_OFFLINE), 0);
    lv_obj_set_style_text_font(s_ui.status_lbl, APP_FONT_TEXT, 0);
    lv_obj_align(s_ui.status_lbl, LV_ALIGN_TOP_RIGHT, -14, 18);
}

static void create_info_block(void)
{
    /* 标题 */
    s_ui.title_lbl = lv_label_create(s_ui.screen);
    lv_label_set_text(s_ui.title_lbl, "Nothing playing");
    lv_obj_set_style_text_color(s_ui.title_lbl, lv_color_hex(COLOR_TEXT), 0);
    lv_obj_set_style_text_font(s_ui.title_lbl, APP_FONT_TITLE, 0);
    lv_label_set_long_mode(s_ui.title_lbl, LV_LABEL_LONG_DOT);
    lv_obj_set_width(s_ui.title_lbl, 220);
    lv_obj_set_style_text_align(s_ui.title_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_ui.title_lbl, LV_ALIGN_TOP_MID, 0, 60);

    /* 歌手 */
    s_ui.artist_lbl = lv_label_create(s_ui.screen);
    lv_label_set_text(s_ui.artist_lbl, "--");
    lv_obj_set_style_text_color(s_ui.artist_lbl, lv_color_hex(COLOR_MUTED), 0);
    lv_obj_set_style_text_font(s_ui.artist_lbl, APP_FONT_TEXT, 0);
    lv_label_set_long_mode(s_ui.artist_lbl, LV_LABEL_LONG_DOT);
    lv_obj_set_width(s_ui.artist_lbl, 220);
    lv_obj_set_style_text_align(s_ui.artist_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_ui.artist_lbl, LV_ALIGN_TOP_MID, 0, 92);
}

static void create_progress_block(void)
{
    /* 进度条：只读，拖动不响应（移除 CLICKABLE） */
    s_ui.slider = lv_slider_create(s_ui.screen);
    lv_obj_set_size(s_ui.slider, 200, 6);
    lv_obj_align(s_ui.slider, LV_ALIGN_TOP_MID, 0, 145);
    lv_obj_remove_flag(s_ui.slider, LV_OBJ_FLAG_CLICKABLE);
    lv_slider_set_range(s_ui.slider, 0, 100);
    lv_slider_set_value(s_ui.slider, 0, LV_ANIM_OFF);

    lv_obj_set_style_bg_color(s_ui.slider, lv_color_hex(COLOR_CARD_ALT), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_ui.slider, lv_color_hex(COLOR_ACCENT), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s_ui.slider, lv_color_hex(COLOR_ACCENT), LV_PART_KNOB);
    lv_obj_set_style_pad_all(s_ui.slider, 4, LV_PART_KNOB);

    /* 时间文字 */
    s_ui.time_lbl = lv_label_create(s_ui.screen);
    lv_label_set_text(s_ui.time_lbl, "--:-- / --:--");
    lv_obj_set_style_text_color(s_ui.time_lbl, lv_color_hex(COLOR_MUTED), 0);
    lv_obj_set_style_text_font(s_ui.time_lbl, APP_FONT_TEXT, 0);
    lv_obj_align(s_ui.time_lbl, LV_ALIGN_TOP_MID, 0, 165);
}

static lv_obj_t *create_ctrl_btn(const char *symbol, lv_obj_t **out_icon,
                                 int x_offset)
{
    lv_obj_t *btn = lv_btn_create(s_ui.screen);
    lv_obj_remove_style_all(btn);
    lv_obj_add_style(btn, &s_ui.style_ctrl_btn, 0);
    lv_obj_add_style(btn, &s_ui.style_ctrl_btn_pressed, LV_STATE_PRESSED);
    lv_obj_set_size(btn, 60, 60);
    lv_obj_align(btn, LV_ALIGN_TOP_MID, x_offset, 225);
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *icon = lv_label_create(btn);
    lv_label_set_text(icon, symbol);
    lv_obj_set_style_text_color(icon, lv_color_hex(COLOR_ACCENT), 0);
    lv_obj_set_style_text_font(icon, APP_FONT_TITLE, 0);
    lv_obj_center(icon);

    if (out_icon) {
        *out_icon = icon;
    }
    return btn;
}

static void create_control_buttons(void)
{
    /* 三个按钮水平居中：中心位置的 x 偏移 -80, 0, +80 （60 + 20 gap） */
    s_ui.prev_btn = create_ctrl_btn(LV_SYMBOL_PREV, NULL, -80);
    s_ui.pp_btn   = create_ctrl_btn(LV_SYMBOL_PLAY, &s_ui.pp_icon, 0);
    s_ui.next_btn = create_ctrl_btn(LV_SYMBOL_NEXT, NULL, 80);

    lv_obj_set_user_data(s_ui.prev_btn, (void *)(uintptr_t)MEDIA_BTN_PREV);
    lv_obj_set_user_data(s_ui.pp_btn,   (void *)(uintptr_t)MEDIA_BTN_PLAY_PAUSE);
    lv_obj_set_user_data(s_ui.next_btn, (void *)(uintptr_t)MEDIA_BTN_NEXT);
}

/* ============================================================================
 * 辅助格式化
 * ========================================================================= */

static void format_mmss(int16_t total_sec, char *out, size_t out_size)
{
    if (total_sec < 0) {
        snprintf(out, out_size, "--:--");
        return;
    }
    int m = total_sec / 60;
    int s = total_sec % 60;
    snprintf(out, out_size, "%d:%02d", m, s);
}

/* ============================================================================
 * 刷新逻辑
 * ========================================================================= */

/* 收到新数据：重设 title/artist/总时长/Play-Pause 图标 + 进度条量程 */
static void refresh_static(const media_payload_t *m)
{
    lv_label_set_text(s_ui.title_lbl,
        (m->title[0] != '\0') ? m->title : "Nothing playing");
    lv_label_set_text(s_ui.artist_lbl,
        (m->artist[0] != '\0') ? m->artist : "--");

    /* Play/Pause 图标切换 */
    lv_label_set_text(s_ui.pp_icon,
        m->playing ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);

    /* 进度条 range */
    if (m->duration_sec > 0) {
        lv_slider_set_range(s_ui.slider, 0, m->duration_sec);
    } else {
        /* 未知时长：用 100 作占位，进度条不会实际推进 */
        lv_slider_set_range(s_ui.slider, 0, 100);
    }
}

/* 每帧：根据 get_position_now() 更新进度条和当前时间文本 */
static void refresh_progress(const media_payload_t *m)
{
    int16_t pos = media_manager_get_position_now();
    if (pos == s_ui.last_pos_sec) {
        return;
    }
    s_ui.last_pos_sec = pos;

    /* 进度条 */
    if (pos >= 0 && m->duration_sec > 0) {
        lv_slider_set_value(s_ui.slider, pos, LV_ANIM_OFF);
    } else {
        lv_slider_set_value(s_ui.slider, 0, LV_ANIM_OFF);
    }

    /* 时间文字 "mm:ss / mm:ss" */
    char cur[8], total[8], buf[20];
    format_mmss(pos, cur, sizeof(cur));
    format_mmss(m->duration_sec, total, sizeof(total));
    snprintf(buf, sizeof(buf), "%s / %s", cur, total);
    lv_label_set_text(s_ui.time_lbl, buf);
}

static void apply_connection_style(bool connected)
{
    lv_label_set_text(s_ui.status_lbl, connected ? "Connected" : "Off");
    lv_obj_set_style_text_color(s_ui.status_lbl,
        lv_color_hex(connected ? COLOR_SUCCESS : COLOR_OFFLINE), 0);

    lv_obj_t *btns[] = { s_ui.prev_btn, s_ui.pp_btn, s_ui.next_btn };
    for (size_t i = 0; i < sizeof(btns) / sizeof(btns[0]); i++) {
        if (connected) {
            lv_obj_remove_style(btns[i], &s_ui.style_ctrl_btn_disabled, 0);
        } else {
            lv_obj_add_style(btns[i], &s_ui.style_ctrl_btn_disabled, 0);
        }
    }
}

/* ============================================================================
 * 事件回调
 * ========================================================================= */

static void on_back_clicked(lv_event_t *e)
{
    (void)e;
    app_router_exit_to_launcher();
}

static void on_ctrl_btn_clicked(lv_event_t *e)
{
    lv_obj_t *btn = lv_event_get_target(e);
    uint8_t id = (uint8_t)(uintptr_t)lv_obj_get_user_data(btn);

    esp_err_t r = media_service_send_button(id);
    if (r == ESP_OK) {
        ESP_LOGI(TAG, "media button sent: id=%u", id);
    } else {
        ESP_LOGW(TAG, "media button send failed: id=%u err=0x%x", id, r);
    }
}

static void bind_events(void)
{
    lv_obj_add_event_cb(s_ui.back_btn, on_back_clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(s_ui.prev_btn, on_ctrl_btn_clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(s_ui.pp_btn,   on_ctrl_btn_clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(s_ui.next_btn, on_ctrl_btn_clicked, LV_EVENT_CLICKED, NULL);
}

/* ============================================================================
 * 页面生命周期
 * ========================================================================= */

static void page_init(void)
{
    init_styles();
    create_top_bar();
    create_info_block();
    create_progress_block();
    create_control_buttons();
    bind_events();

    s_ui.last_version = 0;
    s_ui.last_pos_sec = -2;     /* 强制首帧刷新 */
    s_ui.last_connected = ble_driver_is_connected();
    apply_connection_style(s_ui.last_connected);

    /* 进入页面时若已有快照，立刻渲染一次 */
    const media_payload_t *m = media_manager_get_latest();
    if (m) {
        refresh_static(m);
        refresh_progress(m);
        s_ui.last_version = media_manager_version();
    }
}

static lv_obj_t *page_music_create(void)
{
    ESP_LOGI(TAG, "Creating music page");
    s_ui.screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_ui.screen, lv_color_hex(COLOR_BG), 0);

    page_init();
    return s_ui.screen;
}

static void page_music_destroy(void)
{
    ESP_LOGI(TAG, "Destroying music page");

    if (s_ui.screen) {
        lv_obj_del(s_ui.screen);
        s_ui.screen = NULL;
    }

    lv_style_reset(&s_ui.style_topbtn);
    lv_style_reset(&s_ui.style_topbtn_pressed);
    lv_style_reset(&s_ui.style_ctrl_btn);
    lv_style_reset(&s_ui.style_ctrl_btn_pressed);
    lv_style_reset(&s_ui.style_ctrl_btn_disabled);

    memset(&s_ui, 0, sizeof(s_ui));
}

static void page_music_update(void)
{
    /* 连接状态变更（Play/Pause 图标和连接色改动） */
    bool now_connected = ble_driver_is_connected();
    if (now_connected != s_ui.last_connected) {
        s_ui.last_connected = now_connected;
        apply_connection_style(now_connected);
    }

    /* 静态字段：只在 version 变化时重绘 */
    uint32_t ver = media_manager_version();
    const media_payload_t *m = media_manager_get_latest();

    if (ver != s_ui.last_version) {
        s_ui.last_version = ver;
        s_ui.last_pos_sec = -2;   /* 强制下一次 progress 重绘 */
        if (m) {
            refresh_static(m);
        }
    }

    /* 进度条每帧更新（内部按差值去重） */
    if (m) {
        refresh_progress(m);
    }
}

static const page_callbacks_t s_callbacks = {
    .create  = page_music_create,
    .destroy = page_music_destroy,
    .update  = page_music_update,
};

const page_callbacks_t *page_music_get_callbacks(void)
{
    return &s_callbacks;
}
