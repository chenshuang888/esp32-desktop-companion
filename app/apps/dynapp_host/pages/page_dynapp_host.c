#include "page_dynapp_host.h"

#include <string.h>

#include "app_fonts.h"
#include "app_router.h"
#include "dynamic_app.h"
#include "dynamic_app_ui.h"
#include "esp_log.h"
#include "lvgl.h"

static const char *TAG = "dynapp_host";

/* ============================================================================
 * 动态 App 宿主：
 *
 * 路径 A）后台 prepare + 瞬切（推荐，launcher 用）：
 *   dynapp_host_prepare_and_enter("calc")
 *   → off-screen build subtree + 起脚本
 *   → script ready / 800ms 超时 → app_router_commit_prepared("dynapp_host", screen)
 *
 * 路径 B）同步 enter（兜底）：
 *   dynapp_host_set_pending("calc"); app_router_enter("dynapp_host")
 *   → on_enter 同步 build + start，先切屏后 build（用户看到组件冒出来）
 *
 * 退出：on_exit → 关 root + 停脚本 + 删 screen
 * 返回 launcher：屏内"返回"按钮
 * ========================================================================= */

#define COLOR_BG     0x1E1B2E
#define COLOR_ACCENT 0x06B6D4
#define COLOR_TEXT   0xF1ECFF
#define COLOR_MUTED  0x9B94B5

#define PREPARE_TIMEOUT_MS 800

typedef enum {
    PREP_IDLE = 0,
    PREP_PREPARING,
    PREP_COMMITTED,
} prep_state_t;

typedef struct {
    lv_obj_t *screen;
    lv_obj_t *back_btn;
    lv_obj_t *title_lbl;
    lv_obj_t *list_root;

    lv_style_t style_topbtn;
    lv_style_t style_topbtn_pressed;

    prep_state_t  state;
    lv_timer_t   *timeout_timer;
} ui_t;

static ui_t s_ui = {0};
static char s_pending_app[16] = "";

/* 前向 */
static void build_screen_subtree(void);
static void teardown_subtree(void);
static void on_ready_cb(void *ud);
static void on_timeout_cb(lv_timer_t *t);
static void commit_now(const char *reason);

/* ============================================================================
 * 公开 API
 * ========================================================================= */

void dynapp_host_set_pending(const char *app_name)
{
    if (!app_name || !app_name[0]) return;
    strncpy(s_pending_app, app_name, sizeof(s_pending_app) - 1);
    s_pending_app[sizeof(s_pending_app) - 1] = '\0';
}

void dynapp_host_cancel_prepare_if_any(void)
{
    if (s_ui.state != PREP_PREPARING) return;

    ESP_LOGI(TAG, "cancel prepare for app: %s", s_pending_app);

    dynamic_app_ui_set_root(NULL);
    dynamic_app_stop();
    dynamic_app_ui_unregister_all();

    if (s_ui.timeout_timer) {
        lv_timer_del(s_ui.timeout_timer);
        s_ui.timeout_timer = NULL;
    }

    if (s_ui.screen) {
        lv_obj_del(s_ui.screen);
    }
    s_ui.screen     = NULL;
    s_ui.back_btn   = NULL;
    s_ui.title_lbl  = NULL;
    s_ui.list_root  = NULL;
    lv_style_reset(&s_ui.style_topbtn);
    lv_style_reset(&s_ui.style_topbtn_pressed);

    s_ui.state = PREP_IDLE;
}

void dynapp_host_prepare_and_enter(const char *app_name)
{
    if (!app_name || !app_name[0]) return;

    if (s_ui.state == PREP_PREPARING) dynapp_host_cancel_prepare_if_any();
    if (s_ui.state == PREP_COMMITTED) {
        ESP_LOGW(TAG, "already on dynapp_host, ignore prepare for %s", app_name);
        return;
    }

    dynapp_host_set_pending(app_name);
    ESP_LOGI(TAG, "prepare app in background: %s", s_pending_app);

    build_screen_subtree();

    dynamic_app_ui_set_fonts(APP_FONT_TEXT, APP_FONT_TITLE, APP_FONT_HUGE);
    dynamic_app_ui_set_root(s_ui.list_root);

    dynamic_app_ui_set_ready_cb(on_ready_cb, NULL);
    s_ui.timeout_timer = lv_timer_create(on_timeout_cb, PREPARE_TIMEOUT_MS, NULL);
    lv_timer_set_repeat_count(s_ui.timeout_timer, 1);

    s_ui.state = PREP_PREPARING;
    dynamic_app_start(s_pending_app);
}

/* ============================================================================
 * 内部
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
}

static void on_back_clicked(lv_event_t *e)
{
    (void)e;
    app_router_exit_to_launcher();
}

static void build_screen_subtree(void)
{
    init_styles();

    s_ui.screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_ui.screen, lv_color_hex(COLOR_BG), 0);
    lv_obj_clear_flag(s_ui.screen, LV_OBJ_FLAG_SCROLLABLE);

    s_ui.back_btn = lv_btn_create(s_ui.screen);
    lv_obj_remove_style_all(s_ui.back_btn);
    lv_obj_add_style(s_ui.back_btn, &s_ui.style_topbtn, 0);
    lv_obj_add_style(s_ui.back_btn, &s_ui.style_topbtn_pressed, LV_STATE_PRESSED);
    lv_obj_set_style_radius(s_ui.back_btn, 6, 0);
    lv_obj_set_size(s_ui.back_btn, 90, 30);
    lv_obj_align(s_ui.back_btn, LV_ALIGN_TOP_LEFT, 10, 10);
    lv_obj_add_event_cb(s_ui.back_btn, on_back_clicked, LV_EVENT_CLICKED, NULL);

    lv_obj_t *arrow = lv_label_create(s_ui.back_btn);
    lv_label_set_text(arrow, LV_SYMBOL_LEFT " 返回");
    lv_obj_set_style_text_font(arrow, APP_FONT_TEXT, 0);
    lv_obj_center(arrow);

    s_ui.title_lbl = lv_label_create(s_ui.screen);
    lv_label_set_text(s_ui.title_lbl, "Dynamic App");
    lv_obj_set_style_text_color(s_ui.title_lbl, lv_color_hex(COLOR_TEXT), 0);
    lv_obj_set_style_text_font(s_ui.title_lbl, APP_FONT_TITLE, 0);
    lv_obj_align(s_ui.title_lbl, LV_ALIGN_TOP_MID, 0, 14);

    s_ui.list_root = lv_obj_create(s_ui.screen);
    lv_obj_remove_style_all(s_ui.list_root);
    lv_obj_set_size(s_ui.list_root, 220, 250);
    lv_obj_align(s_ui.list_root, LV_ALIGN_TOP_MID, 0, 50);
    lv_obj_set_style_pad_all(s_ui.list_root, 0, 0);
    lv_obj_set_style_text_color(s_ui.list_root, lv_color_hex(COLOR_TEXT), 0);
    lv_obj_set_style_text_font(s_ui.list_root, APP_FONT_TEXT, 0);
    lv_obj_set_style_bg_opa(s_ui.list_root, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_ui.list_root, 0, 0);
    lv_obj_set_scrollbar_mode(s_ui.list_root, LV_SCROLLBAR_MODE_AUTO);
}

static void teardown_subtree(void)
{
    if (s_ui.screen) {
        lv_obj_del(s_ui.screen);
        s_ui.screen = NULL;
    }
    s_ui.back_btn  = NULL;
    s_ui.title_lbl = NULL;
    s_ui.list_root = NULL;

    lv_style_reset(&s_ui.style_topbtn);
    lv_style_reset(&s_ui.style_topbtn_pressed);
}

static void on_ready_cb(void *ud)
{
    (void)ud;
    if (s_ui.state != PREP_PREPARING) return;
    commit_now("ready");
}

static void on_timeout_cb(lv_timer_t *t)
{
    (void)t;
    if (s_ui.state != PREP_PREPARING) return;
    ESP_LOGW(TAG, "prepare timeout (%dms) for app: %s, force commit",
             PREPARE_TIMEOUT_MS, s_pending_app);
    commit_now("timeout");
}

static void commit_now(const char *reason)
{
    if (s_ui.timeout_timer) {
        lv_timer_del(s_ui.timeout_timer);
        s_ui.timeout_timer = NULL;
    }
    dynamic_app_ui_set_ready_cb(NULL, NULL);

    lv_obj_t *prepared = s_ui.screen;

    esp_err_t err = app_router_commit_prepared("dynapp_host", prepared);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "commit_prepared failed err=0x%x, rolling back", err);
        dynamic_app_ui_set_root(NULL);
        dynamic_app_stop();
        dynamic_app_ui_unregister_all();
        teardown_subtree();
        s_ui.state = PREP_IDLE;
        return;
    }

    s_ui.state = PREP_COMMITTED;
    ESP_LOGI(TAG, "committed dynamic app=%s reason=%s", s_pending_app, reason);
}

/* ============================================================================
 * page_callbacks_t —— 给 dynapp_host_app 壳调
 * ========================================================================= */

static lv_obj_t *create(void)
{
    /* 同步路径兜底（路径 B）。新代码应走 prepare_and_enter（commit 路径）。 */
    if (s_ui.state == PREP_PREPARING || s_ui.state == PREP_COMMITTED) {
        ESP_LOGW(TAG, "create() called while state=%d, returning current screen", s_ui.state);
        s_ui.state = PREP_COMMITTED;
        return s_ui.screen;
    }

    ESP_LOGI(TAG, "Creating dynapp_host (sync path) for: %s", s_pending_app);

    build_screen_subtree();

    dynamic_app_ui_set_fonts(APP_FONT_TEXT, APP_FONT_TITLE, APP_FONT_HUGE);
    dynamic_app_ui_set_root(s_ui.list_root);

    s_ui.state = PREP_COMMITTED;
    dynamic_app_start(s_pending_app);

    return s_ui.screen;
}

static void destroy(void)
{
    ESP_LOGI(TAG, "Destroying dynapp_host");

    dynamic_app_ui_set_root(NULL);
    dynamic_app_stop();
    dynamic_app_ui_unregister_all();

    if (s_ui.timeout_timer) {
        lv_timer_del(s_ui.timeout_timer);
        s_ui.timeout_timer = NULL;
    }

    teardown_subtree();
    s_ui.state = PREP_IDLE;
}

static const page_callbacks_t s_callbacks = {
    .create  = create,
    .destroy = destroy,
    .update  = NULL,
};

const page_callbacks_t *page_dynapp_host_get_callbacks(void)
{
    return &s_callbacks;
}
