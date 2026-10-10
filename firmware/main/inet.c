#include "inet.h"

#include <string.h>

#include "cJSON.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "net.h"

static const char *TAG = "inet";

#define CHECK_MS        (3 * 60 * 1000)
#define RETRY_MS        (30 * 1000)
/* Free, no key, HTTP only - enough to name the ISP behind our public IP */
#define LOOKUP_URL      "http://ip-api.com/json/?fields=status,isp,as,mobile"
#define STARLINK_ASN    "AS14593"   /* SpaceX Services */

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static inet_status_t s_status;

static void set(inet_kind_t k, const char *isp)
{
    portENTER_CRITICAL(&s_mux);
    s_status.kind = k;
    strlcpy(s_status.isp, isp ? isp : "", sizeof(s_status.isp));
    portEXIT_CRITICAL(&s_mux);
}

void inet_get(inet_status_t *out)
{
    portENTER_CRITICAL(&s_mux);
    *out = s_status;
    portEXIT_CRITICAL(&s_mux);
}

const char *inet_kind_name(inet_kind_t k)
{
    switch (k) {
    case INET_OFFLINE: return "No internet";
    case INET_HOME: return "Home";
    case INET_STARLINK: return "Starlink";
    case INET_CELLULAR: return "4G";
    case INET_OTHER: return "Internet";
    default: return "";
    }
}

/* Returns false if the lookup itself failed (no internet). */
static bool lookup(void)
{
    char body[256] = "";
    esp_http_client_config_t cfg = {.url = LOOKUP_URL, .timeout_ms = 8000};
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) {
        return false;
    }
    int len = -1;
    if (esp_http_client_open(c, 0) == ESP_OK) {
        esp_http_client_fetch_headers(c);
        if (esp_http_client_get_status_code(c) == 200) {
            len = esp_http_client_read_response(c, body, sizeof(body) - 1);
        }
    }
    esp_http_client_cleanup(c);
    if (len <= 0) {
        return false;
    }
    body[len] = '\0';

    cJSON *j = cJSON_Parse(body);
    if (!j) {
        return false;
    }
    const cJSON *isp = cJSON_GetObjectItem(j, "isp");
    const cJSON *as = cJSON_GetObjectItem(j, "as");
    const char *isp_s = cJSON_IsString(isp) ? isp->valuestring : "";
    const char *as_s = cJSON_IsString(as) ? as->valuestring : "";
    bool starlink = strncmp(as_s, STARLINK_ASN " ", strlen(STARLINK_ASN) + 1) == 0 || strstr(isp_s, "SpaceX") ||
                    strstr(isp_s, "Starlink");
    bool mobile = cJSON_IsTrue(cJSON_GetObjectItem(j, "mobile"));
    set(starlink ? INET_STARLINK : mobile ? INET_CELLULAR : INET_OTHER, isp_s);
    ESP_LOGI(TAG, "%s (%s, %s)", inet_kind_name(s_status.kind), isp_s, as_s);
    cJSON_Delete(j);
    return true;
}

static void inet_task(void *arg)
{
    char last_ssid[33] = "";
    TickType_t next = 0;
    for (;;) {
        net_status_t ns;
        net_get_status(&ns);
        if (ns.state != NET_STATE_CONNECTED) {
            set(INET_UNKNOWN, NULL);
            last_ssid[0] = '\0';
        } else {
            bool changed = strcmp(last_ssid, ns.ssid) != 0;
            if (changed || xTaskGetTickCount() >= next) {
                strlcpy(last_ssid, ns.ssid, sizeof(last_ssid));
                if (ns.network >= 0 && ns.home) {
                    set(INET_HOME, NULL);
                    next = xTaskGetTickCount() + pdMS_TO_TICKS(CHECK_MS);
                } else if (lookup()) {
                    next = xTaskGetTickCount() + pdMS_TO_TICKS(CHECK_MS);
                } else {
                    set(INET_OFFLINE, NULL);
                    next = xTaskGetTickCount() + pdMS_TO_TICKS(RETRY_MS);
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}

void inet_start(void)
{
    xTaskCreatePinnedToCoreWithCaps(inet_task, "inet", 6144, NULL, 2, NULL, 1, MALLOC_CAP_SPIRAM);
}
