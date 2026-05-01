#include "system_app.h"
#include "page_system.h"
#include "app_fonts.h"

static lv_obj_t *on_enter(void) { return page_system_get_callbacks()->create(); }
static void on_app_exit(void)       { const page_callbacks_t *cb = page_system_get_callbacks(); if (cb->destroy) cb->destroy(); }
static void on_tick(void)       { const page_callbacks_t *cb = page_system_get_callbacks(); if (cb->update) cb->update(); }

const app_descriptor_t SYSTEM_APP = {
    .id              = "system",
    .display_name    = "系统",
    .menu_icon       = ICON_TUNE,
    .menu_icon_color = 0x3C3C43,
    .immersive       = false,
    .show_in_menu    = true,
    .on_enter        = on_enter,
    .on_exit         = on_app_exit,
    .on_tick         = on_tick,
};
