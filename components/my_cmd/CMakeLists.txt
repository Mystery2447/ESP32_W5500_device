idf_component_register(SRCS "my_cmd.c"
                    INCLUDE_DIRS "." "../my_eth" "../my_nvs" "../my_relay"
                    PRIV_REQUIRES my_eth my_nvs my_relay esp_netif console spi_flash driver
                    )
