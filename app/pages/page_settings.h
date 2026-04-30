#pragma once

#include "page_router.h"

/**
 * 设置页：聚合"时间调节 / 亮度 / 关于"三个原本散在菜单里的入口。
 * 列表风格（参考 iOS 设置首页），与菜单页通过下滑/点击互通。
 */
const page_callbacks_t *page_settings_get_callbacks(void);
