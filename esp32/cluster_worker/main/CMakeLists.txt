idf_component_register(SRCS "main.c" INCLUDE_DIRS "."
                        REQUIRES feb_cluster_link nvs_flash esp_wifi esp_netif esp_event esp_driver_uart)
