#include "page_menu.h"
#include "page_menu_modal.h"
#include "page_dynamic_app.h"

#include "esp_log.h"
#include "lvgl.h"

#include "ui_tokens.h"
#include "ui_widgets.h"
#include "ui_anim.h"
#include "ui_statusbar.h"
#include "app_fonts.h"

#include "ble_driver.h"
#include "lcd_panel.h"
#include "backlight_storage.h"
#include "dynamic_app_registry.h"
#include "dynapp_upload_manager.h"
#include "dynapp_fs_worker.h"

#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <sys/stat.h>

static const char *TAG = "page_menu";

/* ============================================================================
 * 数据驱动的九宫格菜单（v2 设计：状态栏 + 3×3 翻页）
 *
 * cell 类型：
 *   STATIC: 内置入口（time/weather/notify/music/system/backlight/about/time_adjust）
 *   DYNAPP: 动态 app（dynamic_app_registry 枚举），点击进入，长按删除
 *
 * 翻页：
 *   每页固定 9 格（3×3），cell 总数 ≤9 时只 1 页，>9 时分多页
 *   翻页用 lv_obj_set_scroll_snap_x(LV_SCROLL_SNAP_CENTER)
 *   底部点指示器
 *
 * 蓝牙状态项：原 page_menu 里有"蓝牙连接状态"项点击 no-op；状态信息已迁到
 *   状态栏（ui_statusbar），九宫格里**不再保留蓝牙单独入口**。
 *   未来真要做"蓝牙详情页"时再加回。
 *
 * 背光项：保留为单独 cell，点击切档；不再显示当前 % 文字（视觉太碎），
 *   想看 % 就长按状态栏（未来扩展）
 * ========================================================================= */

#define CELLS_PER_PAGE   9
#define MAX_DYN_APPS     16
#define MAX_PAGES        4         /* 9 静态 + 16 动态 = 25 → 3 页够用，多留一页 */
#define MAX_TOTAL_CELLS  (CELLS_PER_PAGE * MAX_PAGES)

typedef enum {
    CELL_STATIC_BACKLIGHT,    /* 切背光档 */
    CELL_STATIC_PAGE,         /* 切到 entry->page_id */
    CELL_DYNAPP,              /* 进动态 app；user_data = char* (heap copy of name) */
} cell_kind_t;

typedef struct {
    cell_kind_t   kind;
    page_id_t     page_id;     /* CELL_STATIC_PAGE 时有效 */
    const char   *icon;        /* Material icon UTF-8 字面量；DYNAPP 用 ICON_APPS */
    const char   *label;       /* 显示中文名 */
    lv_color_t    color;       /* icon 颜色（一图一色）*/
    char         *dyn_name;    /* CELL_DYNAPP 时持有 strdup */
    bool          dyn_has_image;     /* 动态 app 是否有 icon.bin */
    char          dyn_icon_path[80]; /* "A:/littlefs/apps/<id>/icon.bin" */
} cell_def_t;

/* ============================================================================
 * UI state
 * ========================================================================= */

typedef struct {
    lv_obj_t *screen;
    lv_obj_t *statusbar;       /* ui_statusbar */
    lv_obj_t *pager;           /* 横向 scroll snap 容器，每页 240 宽 */
    lv_obj_t *dots_box;        /* 底部分页指示器 */

    cell_def_t cells[MAX_TOTAL_CELLS];
    int        cell_count;
    int        page_count;
    int        active_page;    /* 当前显示页（用于点指示器高亮）*/
} page_menu_ui_t;

static page_menu_ui_t s_ui = {0};
static volatile bool  s_dirty = false;

/* 背光四档 */
static const uint8_t BACKLIGHT_STEPS[] = {64, 128, 192, 255};

/* ============================================================================
 * 静态 cell 定义表（顺序即排版顺序）
 *
 * 与原 page_menu 区别：
 *   - 删了"Bluetooth"入口（状态已上状态栏，无独立详情页）
 *   - 删了 backlight 的 "%" 显示（视觉太碎；点一下就切，看 LED 反馈即可）
 *   - 顺序按"高频在前 + 同类型相邻"重排
 * ========================================================================= */

typedef struct {
    cell_kind_t  kind;
    page_id_t    page_id;
    const char  *icon;
    const char  *label;
    uint32_t     color_hex;
} static_cell_def_t;

/* iOS 风一图一色，跟 mockup 配色一致 */
static const static_cell_def_t s_static_defs[] = {
    { CELL_STATIC_PAGE,       PAGE_TIME_ADJUST,    ICON_EDIT_CALENDAR, "时间", 0x5AC8FA },
    { CELL_STATIC_PAGE,       PAGE_WEATHER,        ICON_WEATHER,       "天气", 0xF59E0B },
    { CELL_STATIC_PAGE,       PAGE_NOTIFICATIONS,  ICON_NOTIFICATIONS, "通知", 0xFF3B30 },
    { CELL_STATIC_PAGE,       PAGE_MUSIC,          ICON_MUSIC,         "音乐", 0xAF52DE },
    { CELL_STATIC_PAGE,       PAGE_SYSTEM,         ICON_TUNE,          "系统", 0x3C3C43 },
    { CELL_STATIC_BACKLIGHT,  PAGE_MAX,            ICON_BRIGHTNESS,    "亮度", 0xFF9500 },
    { CELL_STATIC_PAGE,       PAGE_ABOUT,          ICON_INFO,          "关于", 0x6E6E73 },
};
#define STATIC_DEF_COUNT (int)(sizeof(s_static_defs) / sizeof(s_static_defs[0]))

static const lv_color_t DYNAPP_COLOR = LV_COLOR_MAKE(0x34, 0xC7, 0x59);  /* 绿 */

/* 前向声明 */
static void on_cell_clicked(lv_event_t *e);
static void on_cell_long_pressed(lv_event_t *e);
static void on_pager_scroll(lv_event_t *e);
static void rebuild_cells(void);
static void rebuild_layout(void);
static void update_dots(void);

/* ============================================================================
 * 行为
 * ========================================================================= */

static void cycle_backlight(void)
{
    uint8_t cur = lcd_panel_get_backlight();
    int idx = 0;
    int n = (int)(sizeof(BACKLIGHT_STEPS) / sizeof(BACKLIGHT_STEPS[0]));
    for (int i = 0; i < n; i++) {
        if (cur <= BACKLIGHT_STEPS[i]) { idx = i; break; }
        idx = i;
    }
    int next = (idx + 1) % n;
    uint8_t duty = BACKLIGHT_STEPS[next];
    backlight_storage_set(duty);
    lcd_panel_set_backlight(duty);
    ESP_LOGI(TAG, "Backlight -> %d", duty);
}

static void on_cell_clicked(lv_event_t *e)
{
    cell_def_t *c = (cell_def_t *)lv_event_get_user_data(e);
    if (!c) return;

    switch (c->kind) {
    case CELL_STATIC_BACKLIGHT:
        cycle_backlight();
        break;
    case CELL_STATIC_PAGE:
        page_dynamic_app_cancel_prepare_if_any();
        page_router_switch(c->page_id);
        break;
    case CELL_DYNAPP:
        if (c->dyn_name) {
            page_dynamic_app_prepare_and_switch(c->dyn_name);
        }
        break;
    }
}

static void on_local_delete_done(esp_err_t result, void *cb_arg)
{
    (void)cb_arg;
    if (result == ESP_OK || result == ESP_ERR_NOT_FOUND) {
        s_dirty = true;
    } else {
        ESP_LOGW(TAG, "local delete failed: 0x%x", result);
    }
}

static void on_delete_confirmed(void *ud)
{
    const char *name = (const char *)ud;
    if (!name) return;
    if (!dynapp_fs_worker_submit_app_delete(name, on_local_delete_done, NULL)) {
        ESP_LOGW(TAG, "fs_worker queue full, can't delete: %s", name);
    }
}

static void on_cell_long_pressed(lv_event_t *e)
{
    cell_def_t *c = (cell_def_t *)lv_event_get_user_data(e);
    if (!c || c->kind != CELL_DYNAPP || !c->dyn_name) return;

    lv_indev_t *indev = lv_event_get_indev(e);
    if (indev) lv_indev_wait_release(indev);

    menu_modal_show_delete_confirm(c->dyn_name, on_delete_confirmed, c->dyn_name);
}

/* ============================================================================
 * cell 列表构建（数据层，不碰 LVGL）
 * ========================================================================= */

static void cells_clear(void)
{
    for (int i = 0; i < s_ui.cell_count; i++) {
        if (s_ui.cells[i].dyn_name) {
            free(s_ui.cells[i].dyn_name);
            s_ui.cells[i].dyn_name = NULL;
        }
    }
    memset(s_ui.cells, 0, sizeof(s_ui.cells));
    s_ui.cell_count = 0;
}

static void cells_collect(void)
{
    cells_clear();

    /* 静态 cell */
    for (int i = 0; i < STATIC_DEF_COUNT; i++) {
        if (s_ui.cell_count >= MAX_TOTAL_CELLS) break;
        cell_def_t *c = &s_ui.cells[s_ui.cell_count++];
        c->kind     = s_static_defs[i].kind;
        c->page_id  = s_static_defs[i].page_id;
        c->icon     = s_static_defs[i].icon;
        c->label    = s_static_defs[i].label;
        c->color    = lv_color_hex(s_static_defs[i].color_hex);
        c->dyn_name = NULL;
    }

    /* 动态 cell */
    dynamic_app_entry_t entries[MAX_DYN_APPS];
    int n = dynamic_app_registry_list(entries, MAX_DYN_APPS);
    ESP_LOGI(TAG, "dynamic apps discovered: %d", n);
    for (int i = 0; i < n && s_ui.cell_count < MAX_TOTAL_CELLS; i++) {
        cell_def_t *c = &s_ui.cells[s_ui.cell_count++];
        c->kind     = CELL_DYNAPP;
        c->page_id  = PAGE_MAX;
        c->icon     = ICON_APPS;
        c->label    = entries[i].display;
        c->color    = DYNAPP_COLOR;
        c->dyn_name = strdup(entries[i].id);

        /* 检查 app 是否带 icon.bin */
        snprintf(c->dyn_icon_path, sizeof(c->dyn_icon_path),
                 "/littlefs/apps/%s/icon.bin", entries[i].id);
        struct stat st;
        c->dyn_has_image = (stat(c->dyn_icon_path, &st) == 0 && S_ISREG(st.st_mode));
        if (c->dyn_has_image) {
            /* 转成 LVGL FS 路径 "A:/..." 供 lv_image_set_src 使用 */
            char tmp[88];
            snprintf(tmp, sizeof(tmp), "A:%s", c->dyn_icon_path);
            strncpy(c->dyn_icon_path, tmp, sizeof(c->dyn_icon_path) - 1);
            c->dyn_icon_path[sizeof(c->dyn_icon_path) - 1] = '\0';
        }
    }

    /* 算页数 —— 至少 1 页（即使 0 个 app） */
    s_ui.page_count = (s_ui.cell_count + CELLS_PER_PAGE - 1) / CELLS_PER_PAGE;
    if (s_ui.page_count < 1) s_ui.page_count = 1;
}

/* ============================================================================
 * LVGL 视图层
 * ========================================================================= */

/* 单个 cell 视觉：80×88 透明按钮 + 36px 图标 + 14px 标签 */
static lv_obj_t *create_cell_obj(lv_obj_t *parent, cell_def_t *c)
{
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_remove_style_all(btn);
    lv_obj_set_size(btn, 80, 88);
    lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_radius(btn, UI_R_MD, 0);
    lv_obj_set_style_bg_color(btn, UI_C_PANEL_HI, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa  (btn, LV_OPA_60,    LV_STATE_PRESSED);
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);

    /* 内部纵向 flex：图标 + 标签 */
    lv_obj_set_flex_flow(btn, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(btn, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(btn, UI_SP_XS, 0);
    lv_obj_set_style_pad_all(btn, 0, 0);

    /* 图标 —— 动态 app 有 icon.bin 时用 image，否则用字体图标 */
    if (c->kind == CELL_DYNAPP && c->dyn_has_image) {
        lv_obj_t *img = lv_image_create(btn);
        lv_image_set_src(img, c->dyn_icon_path);
        /* 32×32 的 icon.bin scale 1024 显示 36×36 略大，保持 1× 即可 */
    } else {
        lv_obj_t *icon = lv_label_create(btn);
        lv_label_set_text(icon, c->icon);
        lv_obj_set_style_text_font (icon, APP_FONT_ICONS_36, 0);
        lv_obj_set_style_text_color(icon, c->color, 0);
    }

    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, c->label);
    lv_obj_set_style_text_font (lbl, UI_F_LABEL, 0);
    lv_obj_set_style_text_color(lbl, UI_C_TEXT, 0);

    /* 事件回调：cell_def_t* 作为 user_data */
    lv_obj_add_event_cb(btn, on_cell_clicked,      LV_EVENT_CLICKED,      c);
    lv_obj_add_event_cb(btn, on_cell_long_pressed, LV_EVENT_LONG_PRESSED, c);
    return btn;
}

/* 一个 240 宽的 page，内部 3×3 网格 */
static lv_obj_t *create_page_obj(lv_obj_t *parent, int page_idx)
{
    lv_obj_t *page = lv_obj_create(parent);
    lv_obj_remove_style_all(page);
    lv_obj_set_size(page, 240, lv_pct(100));
    lv_obj_set_style_bg_opa(page, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(page, 0, 0);
    lv_obj_clear_flag(page, LV_OBJ_FLAG_SCROLLABLE);

    /* 3 列 ×3 行 grid */
    static int32_t col_dsc[] = { 80, 80, 80, LV_GRID_TEMPLATE_LAST };
    static int32_t row_dsc[] = { 88, 88, 88, LV_GRID_TEMPLATE_LAST };
    lv_obj_set_grid_dsc_array(page, col_dsc, row_dsc);
    lv_obj_set_layout(page, LV_LAYOUT_GRID);

    int start = page_idx * CELLS_PER_PAGE;
    int end   = start + CELLS_PER_PAGE;
    if (end > s_ui.cell_count) end = s_ui.cell_count;

    for (int i = start; i < end; i++) {
        int slot = i - start;
        int row  = slot / 3;
        int col  = slot % 3;
        lv_obj_t *cell = create_cell_obj(page, &s_ui.cells[i]);
        lv_obj_set_grid_cell(cell,
            LV_GRID_ALIGN_CENTER, col, 1,
            LV_GRID_ALIGN_CENTER, row, 1);
    }
    return page;
}

/* ============================================================================
 * 翻页指示器
 * ========================================================================= */

static void update_dots(void)
{
    if (!s_ui.dots_box) return;
    uint32_t n = lv_obj_get_child_count(s_ui.dots_box);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *dot = lv_obj_get_child(s_ui.dots_box, i);
        bool active = ((int)i == s_ui.active_page);
        lv_obj_set_style_bg_color(dot, active ? UI_C_TEXT : UI_C_BORDER, 0);
    }
}

static void create_dots(lv_obj_t *parent)
{
    s_ui.dots_box = lv_obj_create(parent);
    lv_obj_remove_style_all(s_ui.dots_box);
    lv_obj_set_size(s_ui.dots_box, 240, 16);
    lv_obj_align(s_ui.dots_box, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_opa(s_ui.dots_box, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(s_ui.dots_box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(s_ui.dots_box, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_ui.dots_box,
        LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(s_ui.dots_box, 6, 0);

    for (int i = 0; i < s_ui.page_count; i++) {
        lv_obj_t *dot = lv_obj_create(s_ui.dots_box);
        lv_obj_remove_style_all(dot);
        lv_obj_set_size(dot, 6, 6);
        lv_obj_set_style_radius(dot, 1000, 0);  /* 圆 */
        lv_obj_set_style_bg_color(dot, UI_C_BORDER, 0);
        lv_obj_set_style_bg_opa  (dot, LV_OPA_COVER, 0);
    }
    update_dots();
}

static void on_pager_scroll(lv_event_t *e)
{
    int32_t scroll_x = lv_obj_get_scroll_x(s_ui.pager);
    int new_page = (scroll_x + 120) / 240;   /* 半页吸附 */
    if (new_page < 0) new_page = 0;
    if (new_page >= s_ui.page_count) new_page = s_ui.page_count - 1;
    if (new_page != s_ui.active_page) {
        s_ui.active_page = new_page;
        update_dots();
    }
}

/* ============================================================================
 * 主 layout（重建用）
 * ========================================================================= */

static void rebuild_layout(void)
{
    /* 清空 pager + dots，但保留 statusbar */
    if (s_ui.pager)    { lv_obj_del(s_ui.pager);    s_ui.pager = NULL; }
    if (s_ui.dots_box) { lv_obj_del(s_ui.dots_box); s_ui.dots_box = NULL; }

    /* pager 容器：横向 scroll snap */
    s_ui.pager = lv_obj_create(s_ui.screen);
    lv_obj_remove_style_all(s_ui.pager);
    /* 状态栏 24px，分页指示 16px，留 280px 给主体 */
    lv_obj_set_size(s_ui.pager, 240, 280);
    lv_obj_align(s_ui.pager, LV_ALIGN_TOP_MID, 0, 24);
    lv_obj_set_style_bg_opa(s_ui.pager, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(s_ui.pager, 0, 0);
    lv_obj_set_flex_flow(s_ui.pager, LV_FLEX_FLOW_ROW);
    lv_obj_set_scroll_dir(s_ui.pager, LV_DIR_HOR);
    lv_obj_set_scroll_snap_x(s_ui.pager, LV_SCROLL_SNAP_CENTER);
    lv_obj_set_scrollbar_mode(s_ui.pager, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_event_cb(s_ui.pager, on_pager_scroll, LV_EVENT_SCROLL, NULL);

    for (int i = 0; i < s_ui.page_count; i++) {
        create_page_obj(s_ui.pager, i);
    }

    /* 重建后保持当前页（避免重建后跳回 0）*/
    if (s_ui.active_page >= s_ui.page_count) s_ui.active_page = s_ui.page_count - 1;
    if (s_ui.active_page < 0) s_ui.active_page = 0;
    lv_obj_scroll_to_x(s_ui.pager, s_ui.active_page * 240, LV_ANIM_OFF);

    create_dots(s_ui.screen);
}

static void rebuild_cells(void)
{
    cells_collect();
    rebuild_layout();
}

/* ============================================================================
 * 上传完成 → 自动刷新（同原实现）
 * ========================================================================= */

static void on_upload_status(upload_op_t op, upload_result_t result,
                             uint8_t seq, const char *name, uint32_t extra,
                             const uint8_t *list_buf, size_t list_len)
{
    (void)seq; (void)name; (void)extra; (void)list_buf; (void)list_len;
    if (result != UPL_RESULT_OK) return;
    if (op == UPL_OP_END || op == UPL_OP_DELETE) {
        s_dirty = true;
    }
}

/* ============================================================================
 * 上滑退出菜单 → 回锁屏（与 page_time 的"上滑进菜单"互为往返）
 * ========================================================================= */

static void on_screen_gesture(lv_event_t *e)
{
    (void)e;
    lv_indev_t *indev = lv_indev_active();
    if (!indev) return;
    lv_dir_t dir = lv_indev_get_gesture_dir(indev);
    if (dir == LV_DIR_BOTTOM) {
        /* 下滑回锁屏（菜单的"逆向"动作）*/
        page_dynamic_app_cancel_prepare_if_any();
        page_router_switch(PAGE_TIME);
    }
}

/* ============================================================================
 * 页面生命周期
 * ========================================================================= */

static lv_obj_t *page_menu_create(void)
{
    ESP_LOGI(TAG, "Creating menu page");

    s_ui.screen = lv_obj_create(NULL);
    ui_screen_setup(s_ui.screen);

    lv_obj_add_event_cb(s_ui.screen, on_screen_gesture, LV_EVENT_GESTURE, NULL);

    /* 状态栏（顶部 24px）*/
    s_ui.statusbar = ui_statusbar_create(s_ui.screen);

    s_ui.active_page = 0;
    rebuild_cells();

    /* 注册上传 status 观察者 */
    (void)dynapp_upload_manager_register_status_cb(on_upload_status);
    s_dirty = false;
    return s_ui.screen;
}

static void page_menu_destroy(void)
{
    ESP_LOGI(TAG, "Destroying menu page");

    cells_clear();   /* free dyn_name 字符串 */

    if (s_ui.screen) {
        lv_obj_del(s_ui.screen);
        s_ui.screen = NULL;
    }
    s_ui.statusbar = NULL;
    s_ui.pager     = NULL;
    s_ui.dots_box  = NULL;
}

static void page_menu_update(void)
{
    if (s_dirty) {
        s_dirty = false;
        rebuild_cells();
    }
}

static const page_callbacks_t s_callbacks = {
    .create  = page_menu_create,
    .destroy = page_menu_destroy,
    .update  = page_menu_update,
};

const page_callbacks_t *page_menu_get_callbacks(void)
{
    return &s_callbacks;
}
