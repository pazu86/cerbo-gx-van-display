#include "fwupdate.h"

#include <string.h>

#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "net.h"

static const char *TAG = "fw";

#define CONFIRM_MIN_S   15   /* at least this long, with WiFi up */
#define CONFIRM_MAX_S   60   /* ... or this long regardless */
#define FAILED_SHOW_S   8

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static fw_status_t s_st;
static int64_t s_phase_since_us;
static esp_timer_handle_t s_tick;

static void set_phase(fw_phase_t p)
{
    portENTER_CRITICAL(&s_mux);
    s_st.phase = p;
    s_st.pct = 0;
    s_st.secs = 0;
    portEXIT_CRITICAL(&s_mux);
    s_phase_since_us = esp_timer_get_time();
}

/* 1 Hz, in the esp_timer task (internal-RAM stack: allowed to write flash) */
static void tick_cb(void *arg)
{
    fw_status_t st;
    fw_get(&st);
    int elapsed = (int)((esp_timer_get_time() - s_phase_since_us) / 1000000);

    if (st.phase == FW_VERIFYING) {
        net_status_t ns;
        net_get_status(&ns);
        int up = (int)(esp_timer_get_time() / 1000000);
        bool healthy = (up >= CONFIRM_MIN_S && ns.state == NET_STATE_CONNECTED) || up >= CONFIRM_MAX_S;
        if (healthy) {
            esp_ota_mark_app_valid_cancel_rollback();
            ESP_LOGI(TAG, "firmware %s confirmed after %d s", st.version, up);
            set_phase(FW_IDLE);
        } else {
            /* Progress toward the 15 s minimum, then creep toward 60 s */
            int pct = up < CONFIRM_MIN_S ? up * 80 / CONFIRM_MIN_S
                                         : 80 + (up - CONFIRM_MIN_S) * 20 / (CONFIRM_MAX_S - CONFIRM_MIN_S);
            portENTER_CRITICAL(&s_mux);
            s_st.pct = pct;
            s_st.secs = up;
            portEXIT_CRITICAL(&s_mux);
        }
    } else if (st.phase == FW_RESTARTING) {
        int left = st.secs - 1;
        portENTER_CRITICAL(&s_mux);
        s_st.secs = left;
        portEXIT_CRITICAL(&s_mux);
        if (left <= 0) {
            ESP_LOGI(TAG, "restarting into the new firmware");
            esp_restart();
        }
    } else if (st.phase == FW_FAILED && elapsed >= FAILED_SHOW_S) {
        set_phase(FW_IDLE);
    }
}

void fw_init(void)
{
    strlcpy(s_st.version, esp_app_get_description()->version, sizeof(s_st.version));
    s_phase_since_us = esp_timer_get_time();

    esp_ota_img_states_t state;
    if (esp_ota_get_state_partition(esp_ota_get_running_partition(), &state) == ESP_OK &&
        state == ESP_OTA_IMG_PENDING_VERIFY) {
        ESP_LOGI(TAG, "new firmware %s: verifying", s_st.version);
        s_st.phase = FW_VERIFYING;
    }
    const esp_timer_create_args_t args = {.callback = tick_cb, .name = "fw"};
    esp_timer_create(&args, &s_tick);
    esp_timer_start_periodic(s_tick, 1000 * 1000);
}

void fw_get(fw_status_t *out)
{
    portENTER_CRITICAL(&s_mux);
    *out = s_st;
    portEXIT_CRITICAL(&s_mux);
}

bool fw_busy(void)
{
    fw_phase_t p = s_st.phase;
    return p != FW_IDLE && p != FW_FAILED;
}

const char *fw_phase_name(fw_phase_t p)
{
    switch (p) {
    case FW_IDLE: return "idle";
    case FW_RECEIVING: return "receiving";
    case FW_INSTALLING: return "installing";
    case FW_RESTARTING: return "restarting";
    case FW_VERIFYING: return "verifying";
    case FW_FAILED: return "failed";
    }
    return "?";
}

void fw_receive_begin(void)
{
    set_phase(FW_RECEIVING);
}

void fw_receive_progress(int done, int total)
{
    portENTER_CRITICAL(&s_mux);
    s_st.pct = total > 0 ? (int)((int64_t)done * 100 / total) : 0;
    portEXIT_CRITICAL(&s_mux);
}

void fw_installing(void)
{
    set_phase(FW_INSTALLING);
}

void fw_failed(const char *why)
{
    set_phase(FW_FAILED);
    portENTER_CRITICAL(&s_mux);
    strlcpy(s_st.message, why ? why : "", sizeof(s_st.message));
    portEXIT_CRITICAL(&s_mux);
}

void fw_restart_countdown(int secs)
{
    set_phase(FW_RESTARTING);
    portENTER_CRITICAL(&s_mux);
    s_st.secs = secs;
    portEXIT_CRITICAL(&s_mux);
}
