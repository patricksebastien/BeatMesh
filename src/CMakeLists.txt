idf_component_register(SRCS "main.cpp"
                    INCLUDE_DIRS "."
                    REQUIRES LovyanGFX esp_http_server nvs_flash esp_wifi esp_event esp_abl_link)