idf_component_register(SRCS "my_uart.c"
                    INCLUDE_DIRS "."
                    REQUIRES my_cmd
                    REQUIRES my_nvs
                    REQUIRES my_eth
                    REQUIRES console
                    PRIV_REQUIRES driver
                    )
