#pragma once
/*
 * Which internet service the display is using: Home (a network flagged
 * home), Starlink or 4G - worked out from the public IP's provider via
 * ip-api.com, so it follows whichever uplink the van router is actually
 * using - or none.
 */

typedef enum {
    INET_UNKNOWN,     /* not checked yet */
    INET_OFFLINE,     /* WiFi up but no internet */
    INET_HOME,
    INET_STARLINK,
    INET_CELLULAR,
    INET_OTHER,       /* fixed line that isn't home, e.g. campground WiFi */
} inet_kind_t;

typedef struct {
    inet_kind_t kind;
    char isp[32];
} inet_status_t;

void inet_start(void);
void inet_get(inet_status_t *out);
const char *inet_kind_name(inet_kind_t k);
