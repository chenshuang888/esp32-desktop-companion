#include "music_app.h"
#include "page_music.h"
#include "app_fonts.h"

static lv_obj_t *on_enter(void) { return page_music_get_callbacks()->create(); }
static void on_app_exit(void)       { const page_callbacks_t *cb = page_music_get_callbacks(); if (cb->destroy) cb->destroy(); }
static void on_tick(void)       { const page_callbacks_t *cb = page_music_get_callbacks(); if (cb->update) cb->update(); }

const app_descriptor_t MUSIC_APP = {
    .id              = "music",
    .display_name    = "音乐",
    .menu_icon       = ICON_MUSIC,
    .menu_icon_color = 0xAF52DE,
    .immersive       = false,
    .show_in_menu    = true,
    .on_enter        = on_enter,
    .on_exit         = on_app_exit,
    .on_tick         = on_tick,
};
