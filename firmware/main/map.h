#pragma once
/*
 * Slippy map of OpenStreetMap tiles, centred on the van.
 *
 * Tiles are fetched over HTTPS by a background task, decoded once to RGB565
 * in PSRAM and kept in a small LRU pool. map_update() must be called from
 * the LVGL task (e.g. a page refresh); it is cheap when nothing moved.
 */

#include <stdbool.h>
#include "lvgl.h"

#define MAP_W 512
#define MAP_H 512

void map_create(lv_obj_t *parent, int x, int y);

/* lat/lon NAN = unknown. home NAN = no home marker. course in degrees, NAN = no heading. */
void map_update(double lat, double lon, float course_deg, double home_lat, double home_lon);
