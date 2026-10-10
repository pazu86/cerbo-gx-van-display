#pragma once
/*
 * Web interface at http://van-display.local (HTTP basic auth, see
 * secrets.h): settings, health and firmware update (OTA with rollback).
 *
 *   GET  /                 single-page UI
 *   GET  /api/health       JSON status
 *   GET  /api/settings     JSON settings
 *   POST /api/settings     JSON settings (partial updates allowed)
 *   POST /api/ota          raw firmware .bin as the request body
 *   POST /api/restart
 */

void web_start(void);   /* after net_start() */
