idf_component_register(
    SRCS "main.c" "ui.c" "page_overview.c" "page_solar.c" "page_travel.c" "page_settings.c"
         "map.c" "png565.c" "settings.c" "power.c" "inet.c" "web.c" "fwupdate.c" "splash.c"
         "motion.c" "selfupdate.c"
    INCLUDE_DIRS "."
    EMBED_FILES "web/index.html" "splash/splash.jpg"
    REQUIRES board net cerbo pm335 esp_timer esp_netif esp_http_client esp_http_server mbedtls esp_rom
             app_update esp_app_format json nvs_flash
)
