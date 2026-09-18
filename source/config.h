/* config.h -- build-time constants for the Banana Kong host. */
#ifndef BK_CONFIG_H
#define BK_CONFIG_H

#define BK_TITLE          "Banana Kong"
#define BK_DIR_NAME       "bananakong"
#define BK_DEFAULT_DIR    "sdmc:/switch/" BK_DIR_NAME
#define BK_LIB_NAME       "libBananaKong.so"
#define BK_PACKAGE        "com.fdgentertainment.bananakong"
#define BK_VERSION_NAME   "2.0.0"

/* game.projectc: display 1920x1080. Render there in both modes; the handheld
 * panel is downscaled by the compositor. */
#define BK_RENDER_W       1920
#define BK_RENDER_H       1080
#define BK_PANEL_W        1280
#define BK_PANEL_H        720

/* What the fake Android reports. 30 keeps AKEYCODE_BACK in the native input
 * queue (33+ routes back through OnBackInvokedCallback in Java). */
#define BK_SDK_INT        30
#define BK_AUDIO_RATE     48000

#ifndef BK_LOG_ENABLE
#define BK_LOG_ENABLE     0
#endif

#endif
