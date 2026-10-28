#pragma once
/*
 * Travel safety: once the Cerbo's GPS shows the van has really moved (not a
 * GPS jump), make sure the PM335 water pump and hot water are off.
 */

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    bool enabled;             /* Settings "Travel safety" switch; detection runs either way */
    bool anchored;            /* have a reference position */
    float from_anchor_m;      /* distance of the last good fix from it, -1 none */
    int far_s;                /* how long it has been beyond the trigger distance */
    uint32_t trips;           /* times movement was detected since boot */
    uint32_t offs_sent;       /* off commands sent */
    int last_trip_s;          /* seconds since the last detection, -1 never */
} motion_status_t;

void motion_start(void);
void motion_get(motion_status_t *out);
