idf_component_register(
    SRCS "cerbo.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES mqtt net esp_timer json
)
