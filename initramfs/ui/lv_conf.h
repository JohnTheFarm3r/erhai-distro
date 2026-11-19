/* SPDX-License-Identifier: GPL-2.0-only */
/* LVGL settings of erhai-ui; everything not set here keeps its default. */
#ifndef LV_CONF_H
#define LV_CONF_H

#define LV_COLOR_DEPTH 32
#define LV_USE_STDLIB_MALLOC LV_STDLIB_CLIB
#define LV_USE_STDLIB_STRING LV_STDLIB_CLIB
#define LV_USE_STDLIB_SPRINTF LV_STDLIB_CLIB
#define LV_DEF_REFR_PERIOD 16
#define LV_DPI_DEF 280

#define LV_USE_LOG 0
#define LV_USE_LINUX_FBDEV 1
#define LV_LINUX_FBDEV_RENDER_MODE LV_DISPLAY_RENDER_MODE_PARTIAL
#define LV_LINUX_FBDEV_BUFFER_COUNT 2
#define LV_LINUX_FBDEV_BUFFER_SIZE 120
#define LV_USE_EVDEV 1

#define LV_USE_FS_STDIO 1
#define LV_FS_STDIO_LETTER 'A'
#define LV_FS_STDIO_PATH ""
#define LV_FS_STDIO_CACHE_SIZE 0
#define LV_USE_TINY_TTF 1
#define LV_TINY_TTF_FILE_SUPPORT 1
#define LV_TINY_TTF_CACHE_GLYPH_CNT 256

/* The keys of the on-screen keyboard use the symbols of this font. */
#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_MONTSERRAT_48 1
#define LV_FONT_DEFAULT &lv_font_montserrat_48

#define LV_USE_THEME_DEFAULT 1
#define LV_THEME_DEFAULT_DARK 1
#define LV_THEME_DEFAULT_GROW 0
#define LV_THEME_DEFAULT_TRANSITION_TIME 0

#define LV_BUILD_EXAMPLES 0
#define LV_BUILD_DEMOS 0
#endif
