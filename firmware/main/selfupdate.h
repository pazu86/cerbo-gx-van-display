#pragma once
/*
 * Self-update from Home Assistant: the display checks HA for a newer published
 * build shortly after joining a network and every 30 min after that, and
 * installs it itself. Optional: off unless secrets.h has DISPLAY_UPDATE_KEY.
 * Publish with `.\tools\build.ps1 ha`; HA side in homeassistant/.
 */

#include <stddef.h>

void selfupdate_start(void);
void selfupdate_check_now(void);               /* web "Check for update" */
void selfupdate_note(char *out, size_t size);  /* last result, "" before the first check */
