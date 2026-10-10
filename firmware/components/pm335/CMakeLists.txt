idf_component_register(
    SRCS "pm335.c" "pm335_codec.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES bt cerbo nvs_flash esp_timer
)
