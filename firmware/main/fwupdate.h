#pragma once
/*
 * Firmware update lifecycle, shared by the web OTA handler (writer), the
 * on-screen overlay and the web page (readers):
 *
 *   RECEIVING -> INSTALLING -> RESTARTING -> (reboot) -> VERIFYING -> IDLE
 *
 * After booting a new image the bootloader rolls it back on the next reset
 * unless it's confirmed. We confirm once the firmware is demonstrably
 * healthy (running >= 15 s with WiFi up, or 60 s regardless), and refuse
 * restarts / new uploads until then so a confirmation can't be cut short.
 */

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    FW_IDLE,
    FW_RECEIVING,     /* upload in progress */
    FW_INSTALLING,    /* verifying the image, setting the boot partition */
    FW_RESTARTING,    /* countdown to reboot into the new image */
    FW_VERIFYING,     /* running the new image, not yet confirmed */
    FW_FAILED,        /* upload/install failed (shown briefly) */
} fw_phase_t;

typedef struct {
    fw_phase_t phase;
    int pct;          /* RECEIVING: upload %, VERIFYING: progress to confirmation */
    int secs;         /* RESTARTING: countdown; VERIFYING: seconds running */
    char version[32]; /* running firmware */
    char message[48]; /* FW_FAILED reason */
} fw_status_t;

void fw_init(void);                 /* early in app_main */
void fw_get(fw_status_t *out);
bool fw_busy(void);                 /* any phase but IDLE/FAILED: no restarts */
const char *fw_phase_name(fw_phase_t p);

/* Called by the OTA handler */
void fw_receive_begin(void);
void fw_receive_progress(int done, int total);
void fw_installing(void);
void fw_failed(const char *why);
void fw_restart_countdown(int secs); /* reboots when it reaches 0 */
