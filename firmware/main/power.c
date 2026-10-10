#include "power.h"

#include <time.h>

#include "board.h"
#include "esp_log.h"
#include "settings.h"

static const char *TAG = "power";

static power_state_t s_state = POWER_AWAKE;
static lv_obj_t *s_blank;
static bool s_alarm;
static bool s_was_night;
static int s_level = -1;
static int s_preview = -1;

static bool in_night(const settings_t *s)
{
    if (!s->night_enabled) {
        return false;
    }
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    if (tm.tm_year < 120) {
        return false;   /* clock not set yet */
    }
    int m = tm.tm_hour * 60 + tm.tm_min;
    int off = s->night_off_min, on = s->night_on_min;
    if (off == on) {
        return false;
    }
    return off < on ? (m >= off && m < on) : (m >= off || m < on);  /* window may wrap midnight */
}

static void set_state(power_state_t st)
{
    if (st != s_state) {
        ESP_LOGI(TAG, "%s -> %s", power_state_name(), st == POWER_AWAKE ? "awake" : st == POWER_PEEK ? "peek" : "asleep");
        s_state = st;
    }
    lv_obj_set_hidden(s_blank, st != POWER_ASLEEP);
}

static void wake_cb(lv_event_t *e)
{
    settings_t s;
    settings_get(&s);
    lv_display_trigger_activity(NULL);
    set_state(in_night(&s) ? POWER_PEEK : POWER_AWAKE);
}

static void tick_cb(lv_timer_t *t)
{
    settings_t s;
    settings_get(&s);
    bool night = in_night(&s);
    uint32_t idle_ms = lv_display_get_inactive_time(NULL);

    if (s_alarm) {
        set_state(POWER_AWAKE);
    } else if (night) {
        if (!s_was_night) {
            /* Entering the night window: if someone is using it, give them the peek period */
            set_state(idle_ms < s.night_wake_secs * 1000u ? POWER_PEEK : POWER_ASLEEP);
        } else if (s_state != POWER_ASLEEP && idle_ms > s.night_wake_secs * 1000u) {
            set_state(POWER_ASLEEP);
        }
    } else {
        if (s_was_night) {
            /* Morning: back on, idle timeout counts from now */
            lv_display_trigger_activity(NULL);
            set_state(POWER_AWAKE);
        } else if (s_state == POWER_PEEK) {
            set_state(POWER_AWAKE);
        } else if (s_state == POWER_AWAKE && s.idle_timeout_s && idle_ms > s.idle_timeout_s * 1000u) {
            set_state(POWER_ASLEEP);
        }
    }
    s_was_night = night;

    int level;
    if (s_alarm) {
        level = 100;
    } else if (s_state == POWER_ASLEEP) {
        level = 0;
    } else if (s_state == POWER_PEEK) {
        level = s.night_wake_brightness;
    } else {
        level = s_preview >= 0 ? s_preview : s.brightness;
    }
    if (level != s_level) {
        board_backlight_set(level);
        s_level = level;
    }
}

void power_init(void)
{
    s_blank = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_blank);
    lv_obj_set_size(s_blank, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_opa(s_blank, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_blank, lv_color_black(), 0);
    lv_obj_set_clickable(s_blank, true);
    lv_obj_add_event_cb(s_blank, wake_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_set_hidden(s_blank, true);

    lv_timer_create(tick_cb, 200, NULL);
    tick_cb(NULL);
}

void power_set_alarm(bool active)
{
    if (active != s_alarm) {
        s_alarm = active;
        lv_display_trigger_activity(NULL);
    }
}

void power_preview_brightness(int pct)
{
    s_preview = pct;
}

power_state_t power_state(void)
{
    return s_state;
}

const char *power_state_name(void)
{
    return s_state == POWER_AWAKE ? "awake" : s_state == POWER_PEEK ? "peek" : "asleep";
}
