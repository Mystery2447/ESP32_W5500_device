idf_component_register(SRCS "my_nvs.c"
                    INCLUDE_DIRS "." "../my_eth"
                    REQUIRES     my_eth
                    PRIV_REQUIRES driver nvs_flash esp_netif
                    )
