#pragma once

#include "sub_router.h"

/**
 * 获取音乐副屏页面的回调函数。
 * 功能：显示 Windows 当前曲目 + 进度条 + Prev/Play-Pause/Next 控制。
 */
const page_callbacks_t *page_music_get_callbacks(void);
