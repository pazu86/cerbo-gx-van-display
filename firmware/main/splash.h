#pragma once
/*
 * Boot splash (main/splash/splash.jpg, made by tools/make_splash.py): fades
 * in over a black backdrop, holds 5 s while WiFi and the Cerbo connect
 * underneath, then fades out to the Overview. A tap skips it.
 * Call once with the LVGL lock held, after the UI is built.
 *
 * The decoded image borrows 1.2 MB of PSRAM that the map's tile pool takes
 * later: map.c waits for splash_active() to go false before allocating.
 */
#include "lvgl.h"

void splash_start(void);
bool splash_active(void);
