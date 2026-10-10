#pragma once
/*
 * Screen sleep: idle timeout, overnight schedule and touch-to-wake, driving
 * the backlight from the current settings. While asleep a full-screen layer
 * swallows the waking touch so it can't press anything underneath.
 *
 * Day:   on at the set brightness; off after the idle timeout (if any);
 *        a touch wakes it.
 * Night: off; a touch wakes it at the night brightness for night_wake_secs.
 * A fire alarm forces it on at 100 %.
 */

#include <stdbool.h>

typedef enum { POWER_AWAKE, POWER_PEEK, POWER_ASLEEP } power_state_t;

void power_init(void);                  /* call from the LVGL task */
void power_set_alarm(bool active);      /* LVGL task */
void power_preview_brightness(int pct); /* live slider preview, LVGL task */
power_state_t power_state(void);
const char *power_state_name(void);
