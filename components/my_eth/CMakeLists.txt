idf_component_register(SRCS "eth_init.c"
                    INCLUDE_DIRS "." 
                    REQUIRES     esp_netif  
                    PRIV_REQUIRES driver esp_eth esp_event freertos 
                    )
