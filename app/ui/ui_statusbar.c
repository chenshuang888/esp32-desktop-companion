#include "ui_statusbar.h"
#include "ui_tokens.h"
#include "app_fonts.h"
#include "ble_driver.h"
#include "battery_sim.h"

#include <stdio.h>
#include <time.h>

/* ============================================================================
 * 内部状态：每个 statusbar 实例一份
 * ========================================================================= */

typedef struct {
    lv_obj_t *time_lbl;
    lv_obj_t *bt_lbl;
    lv_obj_t *batt_icon_lbl;
    lv_obj_t *batt_pct_lbl;
    lv_timer_t *tick;
} statusbar_ctx_t;

/* ============================================================================
 * 数据 → 显示
 * ========================================================================= */

static const char *batt_icon_for(uint8_t pct)
{
    if (pct >= 80) return ICON_BATT_FULL;
    if (pct >= 60) return ICON_BATT_5BAR;
    if (pct >= 30) return ICON_BATT_3BAR;
    return ICON_BATT_1BAR;
}

static lv_color_t batt_color_for(battery_state_t st)
{
    switch (st) {
    case BATTERY_OK:       return UI_C_OK;
    case BATTERY_LOW:      return UI_C_WARN;
    case BATTERY_CRITICAL: return UI_C_ERR;
    default:               return UI_C_TEXT_MUTED;
    }
}

static void refresh(statusbar_ctx_t *ctx)
{
    /* 时间 */
    time_t now;
    struct tm tm;
    time(&now);
    localtime_r(&now, &tm);
    char tbuf[8];
    snprintf(tbuf, sizeof(tbuf), "%02d:%02d", tm.tm_hour, tm.tm_min);
    lv_label_set_text(ctx->time_lbl, tbuf);

    /* 蓝牙 */
    bool bt_on = ble_driver_is_connected();
    lv_label_set_text(ctx->bt_lbl, bt_on ? ICON_BLUETOOTH : ICON_BT_DISABLED);
    lv_obj_set_style_text_color(ctx->bt_lbl,
        bt_on ? UI_C_ACCENT : UI_C_TEXT_MUTED, 0);

    /* 电池 */
    uint8_t pct = battery_sim_get_percent();
    battery_state_t st = battery_sim_get_state();
    lv_label_set_text(ctx->batt_icon_lbl, batt_icon_for(pct));
    lv_obj_set_style_text_color(ctx->batt_icon_lbl, batt_color_for(st), 0);
    char pbuf[8];
    snprintf(pbuf, sizeof(pbuf), "%u%%", (unsigned)pct);
    lv_label_set_text(ctx->batt_pct_lbl, pbuf);
}

static void tick_cb(lv_timer_t *t)
{
    statusbar_ctx_t *ctx = (statusbar_ctx_t *)lv_timer_get_user_data(t);
    refresh(ctx);
}

static void on_bar_delete(lv_event_t *e)
{
    statusbar_ctx_t *ctx = (statusbar_ctx_t *)lv_event_get_user_data(e);
    if (!ctx) return;
    if (ctx->tick) {
        lv_timer_delete(ctx->tick);
        ctx->tick = NULL;
    }
    lv_free(ctx);
}

/* ============================================================================
 * 公开 API
 * ========================================================================= */

lv_obj_t *ui_statusbar_create(lv_obj_t *parent)
{
    statusbar_ctx_t *ctx = (statusbar_ctx_t *)lv_malloc_zeroed(sizeof(*ctx));
    if (!ctx) return NULL;

    /* 容器：240×24，顶部贴齐，1px 底部分隔线 */
    lv_obj_t *bar = lv_obj_create(parent);
    lv_obj_remove_style_all(bar);
    lv_obj_set_size(bar, 240, 24);
    lv_obj_align(bar, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(bar, UI_C_BG, 0);
    lv_obj_set_style_bg_opa  (bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(bar, 1, 0);
    lv_obj_set_style_border_side (bar, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_color(bar, UI_C_BORDER, 0);
    lv_obj_set_style_border_opa  (bar, LV_OPA_50, 0);
    lv_obj_set_style_pad_hor(bar, UI_SP_MD, 0);
    lv_obj_set_style_pad_ver(bar, 0, 0);

    /* 内部 flex 行：time | spacer(flex_grow) | bt | battery */
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(bar, UI_SP_SM, 0);

    /* 时间（左）*/
    ctx->time_lbl = lv_label_create(bar);
    lv_label_set_text(ctx->time_lbl, "--:--");
    lv_obj_set_style_text_font (ctx->time_lbl, UI_F_LABEL, 0);
    lv_obj_set_style_text_color(ctx->time_lbl, UI_C_TEXT, 0);
    lv_obj_set_flex_grow(ctx->time_lbl, 1);   /* 撑开把右侧推到右边 */

    /* 蓝牙图标（右） —— Material Symbols 18px */
    ctx->bt_lbl = lv_label_create(bar);
    lv_label_set_text(ctx->bt_lbl, ICON_BT_DISABLED);
    lv_obj_set_style_text_font (ctx->bt_lbl, APP_FONT_ICONS_24, 0);
    lv_obj_set_style_text_color(ctx->bt_lbl, UI_C_TEXT_MUTED, 0);

    /* 电池图标 + 百分比 */
    ctx->batt_icon_lbl = lv_label_create(bar);
    lv_label_set_text(ctx->batt_icon_lbl, ICON_BATT_FULL);
    lv_obj_set_style_text_font (ctx->batt_icon_lbl, APP_FONT_ICONS_24, 0);
    lv_obj_set_style_text_color(ctx->batt_icon_lbl, UI_C_OK, 0);

    ctx->batt_pct_lbl = lv_label_create(bar);
    lv_label_set_text(ctx->batt_pct_lbl, "--%");
    lv_obj_set_style_text_font (ctx->batt_pct_lbl, UI_F_LABEL, 0);
    lv_obj_set_style_text_color(ctx->batt_pct_lbl, UI_C_TEXT, 0);

    /* 1 Hz 刷新 */
    ctx->tick = lv_timer_create(tick_cb, 1000, ctx);
    refresh(ctx);   /* 先画一次免得初始 1s 都是 -- */

    /* 销毁时清理 ctx + timer */
    lv_obj_add_event_cb(bar, on_bar_delete, LV_EVENT_DELETE, ctx);

    return bar;
}
