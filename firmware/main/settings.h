#pragma once
/*
 * User settings, persisted in NVS. Changed from the on-screen Settings page
 * or the web interface; readers poll settings_version() to notice changes.
 */

#include <stdbool.h>
#include <stdint.h>

/* Auto is 0 so fresh installs and settings saved by older firmware get it */
typedef enum { THEME_AUTO = 0, THEME_DARK = 1, THEME_DAY = 2 } theme_t;

/* Stored as a blob: only ever append fields (older blobs load with the new
 * fields zeroed). */
typedef struct {
    uint8_t brightness;            /* 5-100 % while awake */
    uint16_t idle_timeout_min;     /* legacy (v1.1 and older); kept in step with idle_timeout_s */
    bool night_enabled;            /* overnight schedule */
    uint16_t night_off_min;        /* minutes after midnight the screen turns off (e.g. 18:00) */
    uint16_t night_on_min;         /* ... and back on (e.g. 07:00) */
    uint8_t night_wake_brightness; /* a touch overnight wakes it at this brightness */
    uint8_t night_wake_secs;       /* ... for this long */
    uint8_t theme;                 /* theme_t; Auto = dark overnight */
    uint16_t idle_timeout_s;       /* screen off after this long untouched; 0 = never */
} settings_t;

void settings_init(void);          /* loads from NVS (or defaults) */
void settings_get(settings_t *out);
void settings_set(const settings_t *s);   /* validates, saves, bumps the version */
uint32_t settings_version(void);
