idf_component_register(SRCS "coex_test.c" INCLUDE_DIRS "."
                        REQUIRES bt esp_wifi esp_netif esp_event nvs_flash esp_timer)
