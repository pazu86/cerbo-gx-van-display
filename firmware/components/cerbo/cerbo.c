#include "cerbo.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mqtt_client.h"
#include "net.h"

static const char *TAG = "cerbo";

#define KEEPALIVE_MS   30000   /* Venus drops subscriptions after 60 s without one */
#define STALE_US       (90LL * 1000 * 1000)
#define CUSTOM_PREFIX  "VanDisplay/"
#define TRAVEL_TOPIC   "VanDisplay/travel"
/* No broker on this network within this long: try another network */
#define UNREACHABLE_US (25LL * 1000 * 1000)
#define AVOID_MS       (10 * 60 * 1000)

static SemaphoreHandle_t s_lock;
static SemaphoreHandle_t s_client_lock;   /* s_client lifetime vs cerbo_publish() from other tasks */
static cerbo_data_t s_data;
static esp_mqtt_client_handle_t s_client;
static bool s_mqtt_up;
static bool s_subscribed;
static int64_t s_last_keepalive_us;
static int64_t s_unreachable_since_us;   /* 0 = broker up (or not trying) */

/* Under the "N/<portal>/" prefix. %s is not used; the portal goes in front. */
static const char *const s_subs[] = {
    "system/0/Dc/Battery/+",
    "system/0/Dc/Pv/Power",
    "system/0/Dc/System/Power",
    "system/0/Dc/System/Current",
    "solarcharger/+/Yield/Power",
    "solarcharger/+/Pv/V",
    "solarcharger/+/History/Daily/0/Yield",
    "solarcharger/+/State",
    "solarcharger/+/Dc/0/Current",
    "solarcharger/+/History/Daily/+/Yield",
    "solarcharger/+/History/Daily/0/MaxPower",
    "solarcharger/+/Yield/User",
    "solarcharger/+/ErrorCode",
    "solarcharger/+/CustomName",
    "solarcharger/+/ProductName",
    "dcdc/+/Dc/In/V",
    "dcdc/+/Dc/0/Power",
    "dcdc/+/State",
    "dcdc/+/CustomName",
    "dcdc/+/ProductName",
    "temperature/+/Temperature",
    "temperature/+/Humidity",
    "temperature/+/TemperatureType",
    "temperature/+/CustomName",
    "temperature/+/ProductName",
    "digitalinput/+/State",
    "digitalinput/+/Alarm",
    "digitalinput/+/CustomName",
    "digitalinput/+/ProductName",
    "gps/0/Fix",
    "gps/0/Position/+",
    "gps/0/Speed",
    "gps/0/Course",
    "tank/+/Level",
    "tank/+/Remaining",
    "tank/+/Capacity",
    "tank/+/FluidType",
    "tank/+/Status",
    "tank/+/CustomName",
    "tank/+/ProductName",
};

/* ------------------------------------------------------------------ */
/* Payload parsing: Venus sends {"value": <number|string|null>}        */
/* ------------------------------------------------------------------ */

typedef struct {
    bool is_num;
    bool is_str;
    double num;
    char str[CERBO_NAME_LEN];
} value_t;

static void parse_value(const char *p, int len, value_t *v)
{
    memset(v, 0, sizeof(*v));
    v->num = NAN;
    char buf[160];
    if (len >= (int)sizeof(buf)) {
        return;  /* arrays/objects we don't use */
    }
    memcpy(buf, p, len);
    buf[len] = '\0';

    const char *s = strstr(buf, "\"value\"");
    if (s) {
        s = strchr(s + 7, ':');
        if (!s) {
            return;
        }
        s++;
    } else {
        s = buf;  /* bare value, e.g. from Node-RED */
    }
    while (*s == ' ') {
        s++;
    }
    if (*s == '"') {
        s++;
        const char *e = strchr(s, '"');
        size_t n = e ? (size_t)(e - s) : strlen(s);
        if (n >= sizeof(v->str)) {
            n = sizeof(v->str) - 1;
        }
        memcpy(v->str, s, n);
        v->str[n] = '\0';
        v->is_str = true;
    } else if (*s == '-' || (*s >= '0' && *s <= '9')) {
        v->num = strtod(s, NULL);
        v->is_num = true;
    }
}

static float as_f(const value_t *v) { return v->is_num ? (float)v->num : NAN; }
static int as_i(const value_t *v) { return v->is_num ? (int)v->num : -1; }

/* ------------------------------------------------------------------ */
/* Device slots                                                       */
/* ------------------------------------------------------------------ */

/* Find the slot for an instance, claiming a free one if needed.
 * Slots are arrays of structs whose first member is a cerbo_dev_t. */
static cerbo_dev_t *slot(void *base, size_t stride, int count, int instance)
{
    cerbo_dev_t *free_slot = NULL;
    for (int i = 0; i < count; i++) {
        cerbo_dev_t *d = (cerbo_dev_t *)((uint8_t *)base + i * stride);
        if (d->instance == instance) {
            return d;
        }
        if (d->instance < 0 && !free_slot) {
            free_slot = d;
        }
    }
    if (free_slot) {
        free_slot->instance = instance;
        free_slot->name[0] = '\0';
    }
    return free_slot;
}

/* CustomName wins over ProductName; ProductName only fills an empty name. */
static bool set_name(cerbo_dev_t *d, const char *leaf, const value_t *v)
{
    if (strcmp(leaf, "CustomName") == 0) {
        if (v->is_str && v->str[0]) {
            strlcpy(d->name, v->str, sizeof(d->name));
        }
        return true;
    }
    if (strcmp(leaf, "ProductName") == 0) {
        if (v->is_str && !d->name[0]) {
            strlcpy(d->name, v->str, sizeof(d->name));
        }
        return true;
    }
    return false;
}

#define SLOT(arr, inst) slot((arr), sizeof((arr)[0]), sizeof(arr) / sizeof((arr)[0]), (inst))

/* ------------------------------------------------------------------ */
/* Topic dispatch (called with s_lock held)                            */
/* ------------------------------------------------------------------ */

static void handle_venus(const char *service, int inst, const char *path, const value_t *v)
{
    cerbo_data_t *d = &s_data;

    if (strcmp(service, "system") == 0) {
        if (strncmp(path, "Dc/Battery/", 11) == 0) {
            const char *k = path + 11;
            if (!strcmp(k, "Soc")) d->soc = as_f(v);
            else if (!strcmp(k, "Voltage")) d->batt_v = as_f(v);
            else if (!strcmp(k, "Current")) d->batt_a = as_f(v);
            else if (!strcmp(k, "Power")) d->batt_w = as_f(v);
            else if (!strcmp(k, "TimeToGo")) d->time_to_go_s = as_f(v);
            else if (!strcmp(k, "ConsumedAmphours")) d->consumed_ah = as_f(v);
            else if (!strcmp(k, "State")) d->batt_state = as_i(v);
        } else if (!strcmp(path, "Dc/Pv/Power")) {
            d->pv_total_w = as_f(v);
        } else if (!strcmp(path, "Dc/System/Power")) {
            d->dc_system_w = as_f(v);
        } else if (!strcmp(path, "Dc/System/Current")) {
            d->dc_system_a = as_f(v);
        }
    } else if (strcmp(service, "solarcharger") == 0) {
        cerbo_solar_t *s = (cerbo_solar_t *)SLOT(d->solar, inst);
        if (!s || set_name(&s->dev, path, v)) return;
        int day;
        if (!strcmp(path, "Yield/Power")) s->pv_w = as_f(v);
        else if (!strcmp(path, "Pv/V")) s->pv_v = as_f(v);
        else if (!strcmp(path, "Dc/0/Current")) s->out_a = as_f(v);
        else if (!strcmp(path, "History/Daily/0/MaxPower")) s->max_w_today = as_f(v);
        else if (!strcmp(path, "Yield/User")) s->yield_total_kwh = as_f(v);
        else if (!strcmp(path, "State")) s->state = as_i(v);
        else if (!strcmp(path, "ErrorCode")) s->error = as_i(v);
        else if (sscanf(path, "History/Daily/%d/Yield", &day) == 1 && day >= 0) {
            if (day < CERBO_SOLAR_DAYS) s->daily_kwh[day] = as_f(v);
            if (day == 0) s->yield_today_kwh = as_f(v);
        }
    } else if (strcmp(service, "dcdc") == 0) {
        if (d->dcdc.instance < 0) {
            d->dcdc.instance = inst;
        }
        if (inst != d->dcdc.instance || set_name(&d->dcdc, path, v)) return;
        if (!strcmp(path, "Dc/In/V")) d->dcdc_in_v = as_f(v);
        else if (!strcmp(path, "Dc/0/Power")) d->dcdc_out_w = as_f(v);
        else if (!strcmp(path, "State")) d->dcdc_state = as_i(v);
    } else if (strcmp(service, "temperature") == 0) {
        cerbo_temp_t *t = (cerbo_temp_t *)SLOT(d->temps, inst);
        if (!t || set_name(&t->dev, path, v)) return;
        if (!strcmp(path, "Temperature")) t->temp_c = as_f(v);
        else if (!strcmp(path, "Humidity")) t->humidity = as_f(v);
        else if (!strcmp(path, "TemperatureType")) t->type = as_i(v);
    } else if (strcmp(service, "digitalinput") == 0) {
        cerbo_input_t *in = (cerbo_input_t *)SLOT(d->inputs, inst);
        if (!in || set_name(&in->dev, path, v)) return;
        if (!strcmp(path, "State")) in->state = as_i(v);
        else if (!strcmp(path, "Alarm")) in->alarm = as_i(v);
    } else if (strcmp(service, "tank") == 0) {
        cerbo_tank_t *t = (cerbo_tank_t *)SLOT(d->tanks, inst);
        if (!t || set_name(&t->dev, path, v)) return;
        if (!strcmp(path, "Level")) t->level_pct = as_f(v);
        else if (!strcmp(path, "Remaining")) t->remaining = as_f(v);
        else if (!strcmp(path, "Capacity")) t->capacity = as_f(v);
        else if (!strcmp(path, "FluidType")) t->fluid = as_i(v);
        else if (!strcmp(path, "Status")) t->status = as_i(v);
    } else if (strcmp(service, "gps") == 0) {
        if (!strcmp(path, "Fix")) d->gps_fix = as_i(v);
        else if (!strcmp(path, "Position/Latitude")) d->lat = v->is_num ? v->num : NAN;
        else if (!strcmp(path, "Position/Longitude")) d->lon = v->is_num ? v->num : NAN;
        else if (!strcmp(path, "Speed")) d->speed_kmh = v->is_num ? (float)(v->num * 3.6) : NAN;
        else if (!strcmp(path, "Course")) d->course_deg = as_f(v);
    }
}

/* ---- VanDisplay/travel from the Node-RED feed tab ---- */

static float jf(const cJSON *o, const char *k)
{
    const cJSON *i = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsNumber(i) ? (float)i->valuedouble : NAN;
}

static int ji(const cJSON *o, const char *k, int dflt)
{
    const cJSON *i = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsNumber(i) ? i->valueint : dflt;
}

static bool jb(const cJSON *o, const char *k)
{
    return cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(o, k));
}

static void js(const cJSON *o, const char *k, char *dst, size_t n)
{
    const cJSON *i = cJSON_GetObjectItemCaseSensitive(o, k);
    strlcpy(dst, cJSON_IsString(i) ? i->valuestring : "", n);
}

static void parse_trip(const cJSON *o, cerbo_trip_t *t)
{
    memset(t, 0, sizeof(*t));
    js(o, "name", t->name, sizeof(t->name));
    t->km = jf(o, "km");
    t->nights = ji(o, "nights", 0);
    js(o, "end", t->end, sizeof(t->end));
    js(o, "dur", t->dur, sizeof(t->dur));
    t->fuel_l = jf(o, "fuel_l");
}

/* Parses into *tr (caller holds s_lock). Leaves tr untouched on bad JSON. */
static void handle_travel(const char *payload, int len, cerbo_travel_t *tr)
{
    cJSON *root = cJSON_ParseWithLength(payload, len);
    if (!root) {
        ESP_LOGW(TAG, "bad travel JSON");
        return;
    }
    memset(tr, 0, sizeof(*tr));
    tr->valid = true;
    tr->rx_us = esp_timer_get_time();
    tr->odo_km = jf(root, "odo_km");
    tr->at_home = jb(root, "at_home");

    const cJSON *sv = cJSON_GetObjectItemCaseSensitive(root, "service");
    tr->service_km_left = jf(sv, "km_left");
    tr->service_pct = ji(sv, "pct", -1);
    tr->service_days_left = ji(sv, "days_left", INT32_MIN);
    js(sv, "next_date", tr->service_next, sizeof(tr->service_next));
    tr->service_due = jb(sv, "due");
    tr->service_warn = jb(sv, "warn");

    const cJSON *home = cJSON_GetObjectItemCaseSensitive(root, "home");
    tr->home_lat = cJSON_IsObject(home) ? jf(home, "lat") : NAN;
    tr->home_lon = cJSON_IsObject(home) ? jf(home, "lon") : NAN;

    const cJSON *trip = cJSON_GetObjectItemCaseSensitive(root, "trip");
    tr->trip_active = jb(trip, "active");
    tr->trip_km = jf(trip, "km");
    tr->trip_fuel_l = jf(trip, "fuel_l");
    tr->trip_max_kmh = jf(trip, "max_kmh");
    tr->trip_max_from_home_km = jf(trip, "max_from_home_km");
    js(trip, "start", tr->trip_start, sizeof(tr->trip_start));

    const cJSON *last = cJSON_GetObjectItemCaseSensitive(root, "last");
    if (cJSON_IsObject(last)) {
        parse_trip(last, &tr->last);
    }
    const cJSON *it;
    cJSON_ArrayForEach(it, cJSON_GetObjectItemCaseSensitive(root, "recent")) {
        if (tr->recent_count < CERBO_MAX_RECENT) {
            parse_trip(it, &tr->recent[tr->recent_count++]);
        }
    }
    const cJSON *st = cJSON_GetObjectItemCaseSensitive(root, "stats");
    tr->stats_trips = ji(st, "trips", 0);
    tr->stats_nights = ji(st, "nights", 0);
    tr->stats_km = jf(st, "km");
    tr->stats_longest_km = jf(st, "longest_km");
    cJSON_ArrayForEach(it, cJSON_GetObjectItemCaseSensitive(root, "monthly")) {
        if (tr->monthly_count < CERBO_MAX_MONTHS) {
            js(it, "m", tr->monthly[tr->monthly_count].m, sizeof(tr->monthly[0].m));
            tr->monthly[tr->monthly_count++].km = jf(it, "km");
        }
    }
    cJSON_Delete(root);
}

static void handle_custom(const char *key, const char *payload, int len)
{
    value_t v;
    parse_value(payload, len, &v);
    char text[sizeof(s_data.custom[0].value)];
    if (v.is_str) {
        strlcpy(text, v.str, sizeof(text));
    } else if (v.is_num) {
        snprintf(text, sizeof(text), "%g", v.num);
    } else {
        snprintf(text, sizeof(text), "%.*s", len < (int)sizeof(text) - 1 ? len : (int)sizeof(text) - 1, payload);
    }

    cerbo_custom_t *free_slot = NULL;
    for (int i = 0; i < CERBO_MAX_CUSTOM; i++) {
        cerbo_custom_t *c = &s_data.custom[i];
        if (strcmp(c->topic, key) == 0) {
            strlcpy(c->value, text, sizeof(c->value));
            return;
        }
        if (!c->topic[0] && !free_slot) {
            free_slot = c;
        }
    }
    if (free_slot) {
        strlcpy(free_slot->topic, key, sizeof(free_slot->topic));
        strlcpy(free_slot->value, text, sizeof(free_slot->value));
    }
}

/* ------------------------------------------------------------------ */
/* MQTT                                                               */
/* ------------------------------------------------------------------ */

static void send_keepalive(bool full)
{
    char topic[48];
    snprintf(topic, sizeof(topic), "R/%s/keepalive", s_data.portal);
    /* An empty keepalive makes Venus republish every value once (initial
     * snapshot); after that only changes are needed. */
    const char *payload = full ? "" : "{\"keepalive-options\": [\"suppress-republish\"]}";
    esp_mqtt_client_publish(s_client, topic, payload, 0, 0, 0);
    s_last_keepalive_us = esp_timer_get_time();
}

static void subscribe_all(void)
{
    char topic[96];
    for (size_t i = 0; i < sizeof(s_subs) / sizeof(s_subs[0]); i++) {
        snprintf(topic, sizeof(topic), "N/%s/%s", s_data.portal, s_subs[i]);
        esp_mqtt_client_subscribe(s_client, topic, 0);
    }
    esp_mqtt_client_subscribe(s_client, CUSTOM_PREFIX "#", 0);
    s_subscribed = true;
    send_keepalive(true);
    ESP_LOGI(TAG, "subscribed (portal %s)", s_data.portal);
}

static void on_data(esp_mqtt_event_handle_t ev)
{
    /* Ignore fragmented messages: nothing we subscribe to is that large */
    if (ev->current_data_offset != 0 || ev->data_len != ev->total_data_len || ev->topic_len <= 0) {
        return;
    }
    char topic[128];
    int tl = ev->topic_len < (int)sizeof(topic) - 1 ? ev->topic_len : (int)sizeof(topic) - 1;
    memcpy(topic, ev->topic, tl);
    topic[tl] = '\0';

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_data.last_rx_us = esp_timer_get_time();
    s_data.rx_count++;

    if (strcmp(topic, TRAVEL_TOPIC) == 0) {
        handle_travel(ev->data, ev->data_len, &s_data.travel);
    } else if (strncmp(topic, CUSTOM_PREFIX, strlen(CUSTOM_PREFIX)) == 0) {
        handle_custom(topic + strlen(CUSTOM_PREFIX), ev->data, ev->data_len);
    } else if (topic[0] == 'N' && topic[1] == '/') {
        /* N/<portal>/<service>/<instance>/<path...> */
        char *portal = topic + 2;
        char *service = strchr(portal, '/');
        if (service) {
            *service++ = '\0';
            char *inst = strchr(service, '/');
            if (inst) {
                *inst++ = '\0';
                char *path = strchr(inst, '/');
                if (path) {
                    *path++ = '\0';
                    if (!s_data.portal[0] && strcmp(service, "system") == 0 && strcmp(path, "Serial") == 0) {
                        strlcpy(s_data.portal, portal, sizeof(s_data.portal));
                        ESP_LOGI(TAG, "portal id %s", s_data.portal);
                    } else {
                        value_t v;
                        parse_value(ev->data, ev->data_len, &v);
                        handle_venus(service, atoi(inst), path, &v);
                    }
                }
            }
        }
    }
    bool need_subscribe = s_data.portal[0] && !s_subscribed;
    xSemaphoreGive(s_lock);

    if (need_subscribe) {
        subscribe_all();
    }
}

static void on_mqtt_event(void *arg, esp_event_base_t base, int32_t id, void *event_data)
{
    esp_mqtt_event_handle_t ev = event_data;
    switch ((esp_mqtt_event_id_t)id) {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "broker connected");
        s_mqtt_up = true;
        s_subscribed = false;
        if (s_data.portal[0]) {
            subscribe_all();
        } else {
            /* The portal ID arrives on N/<portal>/system/0/Serial */
            esp_mqtt_client_subscribe(s_client, "N/+/system/0/Serial", 0);
        }
        break;
    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "broker disconnected");
        s_mqtt_up = false;
        s_subscribed = false;
        break;
    case MQTT_EVENT_DATA:
        on_data(ev);
        break;
    default:
        break;
    }
}

static void client_stop(void)
{
    xSemaphoreTake(s_client_lock, portMAX_DELAY);
    if (s_client) {
        esp_mqtt_client_destroy(s_client);  /* also stops it */
        s_client = NULL;
    }
    s_mqtt_up = false;
    s_subscribed = false;
    xSemaphoreGive(s_client_lock);
}

static void client_start(const char *host)
{
    char uri[64];
    snprintf(uri, sizeof(uri), "mqtt://%s:1883", host);
    esp_mqtt_client_config_t cfg = {
        .broker.address.uri = uri,
        .session.keepalive = 30,
        .network.reconnect_timeout_ms = 5000,
        .buffer.size = 4096,   /* travel summary is ~1.1 kB; lands in PSRAM */
        .buffer.out_size = 512,
        .task.stack_size = 4096,
    };
    xSemaphoreTake(s_client_lock, portMAX_DELAY);
    s_client = esp_mqtt_client_init(&cfg);
    if (!s_client) {
        xSemaphoreGive(s_client_lock);
        ESP_LOGE(TAG, "mqtt init failed");
        return;
    }
    esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID, on_mqtt_event, NULL);
    esp_mqtt_client_start(s_client);
    xSemaphoreGive(s_client_lock);
    s_unreachable_since_us = esp_timer_get_time();
    ESP_LOGI(TAG, "connecting to %s", uri);
}

static void reset_data(void)
{
    char portal[sizeof(s_data.portal)];
    strlcpy(portal, s_data.portal, sizeof(portal));
    memset(&s_data, 0, sizeof(s_data));
    strlcpy(s_data.portal, portal, sizeof(s_data.portal));

    float *nanf_fields[] = {
        &s_data.soc, &s_data.batt_v, &s_data.batt_a, &s_data.batt_w, &s_data.time_to_go_s,
        &s_data.consumed_ah, &s_data.pv_total_w, &s_data.dc_system_w, &s_data.dc_system_a, &s_data.dcdc_in_v,
        &s_data.dcdc_out_w, &s_data.speed_kmh, &s_data.course_deg,
    };
    for (size_t i = 0; i < sizeof(nanf_fields) / sizeof(nanf_fields[0]); i++) {
        *nanf_fields[i] = NAN;
    }
    s_data.lat = s_data.lon = NAN;
    s_data.batt_state = s_data.dcdc_state = s_data.gps_fix = -1;
    s_data.dcdc.instance = -1;
    for (int i = 0; i < CERBO_MAX_SOLAR; i++) {
        cerbo_solar_t *s = &s_data.solar[i];
        *s = (cerbo_solar_t){.dev.instance = -1, .pv_w = NAN, .pv_v = NAN, .out_a = NAN,
                             .yield_today_kwh = NAN, .max_w_today = NAN, .yield_total_kwh = NAN,
                             .state = -1, .error = -1};
        for (int k = 0; k < CERBO_SOLAR_DAYS; k++) {
            s->daily_kwh[k] = NAN;
        }
    }
    for (int i = 0; i < CERBO_MAX_TANKS; i++) {
        s_data.tanks[i] = (cerbo_tank_t){.dev.instance = -1, .level_pct = NAN, .remaining = NAN,
                                         .capacity = NAN, .fluid = -1, .status = -1};
    }
    for (int i = 0; i < CERBO_MAX_TEMPS; i++) {
        s_data.temps[i] = (cerbo_temp_t){.dev.instance = -1, .temp_c = NAN, .humidity = NAN, .type = -1};
    }
    for (int i = 0; i < CERBO_MAX_INPUTS; i++) {
        s_data.inputs[i] = (cerbo_input_t){.dev.instance = -1, .state = -1, .alarm = -1};
    }
}

static void cerbo_task(void *arg)
{
    char host[sizeof(s_data.host)] = "";
    for (;;) {
        net_status_t ns;
        net_get_status(&ns);
        const char *want = (ns.state == NET_STATE_CONNECTED && ns.cerbo) ? ns.cerbo : NULL;

        if (!want) {
            if (s_client) {
                ESP_LOGI(TAG, "network down, stopping");
                client_stop();
            }
            host[0] = '\0';
        } else if (!s_client || strcmp(host, want) != 0) {
            /* First connection, or we moved networks (different Cerbo address) */
            client_stop();
            strlcpy(host, want, sizeof(host));
            xSemaphoreTake(s_lock, portMAX_DELAY);
            reset_data();
            strlcpy(s_data.host, host, sizeof(s_data.host));
            xSemaphoreGive(s_lock);
            client_start(host);
        } else if (s_subscribed && esp_timer_get_time() - s_last_keepalive_us > KEEPALIVE_MS * 1000LL) {
            send_keepalive(false);
        }

        /* Follow the Cerbo: if the broker isn't answering on this network,
         * it is probably on another one we also know about. */
        if (s_client && s_mqtt_up) {
            s_unreachable_since_us = 0;
        } else if (s_client) {
            if (!s_unreachable_since_us) {
                s_unreachable_since_us = esp_timer_get_time();
            } else if (esp_timer_get_time() - s_unreachable_since_us > UNREACHABLE_US) {
                ESP_LOGW(TAG, "no broker at %s", host);
                net_avoid_current(AVOID_MS);
                s_unreachable_since_us = esp_timer_get_time();
            }
        }

        xSemaphoreTake(s_lock, portMAX_DELAY);
        if (!s_client) {
            s_data.state = CERBO_DISCONNECTED;
        } else if (!s_mqtt_up) {
            s_data.state = CERBO_CONNECTING;
        } else if (!s_data.last_rx_us || !s_subscribed) {
            s_data.state = CERBO_CONNECTED;
        } else if (esp_timer_get_time() - s_data.last_rx_us > STALE_US) {
            s_data.state = CERBO_STALE;
        } else {
            s_data.state = CERBO_LIVE;
        }
        bool ok = s_data.state == CERBO_LIVE;
        xSemaphoreGive(s_lock);
        net_set_link_ok(ok);

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void cerbo_start(void)
{
    s_lock = xSemaphoreCreateMutex();
    s_client_lock = xSemaphoreCreateMutex();
    reset_data();
    xTaskCreate(cerbo_task, "cerbo", 3072, NULL, 4, NULL);
}

void cerbo_get(cerbo_data_t *out)
{
    if (!s_lock) {  /* not started yet */
        memset(out, 0, sizeof(*out));
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_data;
    xSemaphoreGive(s_lock);
}

bool cerbo_publish(const char *topic, const char *data, int len, bool retain)
{
    if (!s_client_lock) {
        return false;
    }
    bool ok = false;
    xSemaphoreTake(s_client_lock, portMAX_DELAY);
    if (s_client && s_mqtt_up) {
        /* Queued in the outbox and sent by the MQTT task: never blocks the caller */
        ok = esp_mqtt_client_enqueue(s_client, topic, data, len, 0, retain, true) >= 0;
    }
    xSemaphoreGive(s_client_lock);
    return ok;
}

const char *cerbo_state_name(cerbo_state_t state)
{
    switch (state) {
    case CERBO_DISCONNECTED: return "offline";
    case CERBO_CONNECTING: return "connecting";
    case CERBO_CONNECTED: return "waiting for data";
    case CERBO_LIVE: return "live";
    case CERBO_STALE: return "stale";
    }
    return "?";
}

const char *cerbo_charger_state_name(int state)
{
    switch (state) {
    case 0: return "Off";
    case 2: return "Fault";
    case 3: return "Bulk";
    case 4: return "Absorption";
    case 5: return "Float";
    case 6: return "Storage";
    case 7: return "Equalize";
    case 11: return "Power supply";
    case 245: return "Starting";
    case 247: return "Auto equalize";
    case 252: return "Ext. control";
    default: return "-";
    }
}
