#include "relay.h"
#include "driver/gpio.h"
#include "esp_log.h"
const char *TAG = "RELAY";


void relay_init(void)
{
    gpio_config_t io_conf = {
        .intr_type = GPIO_INTR_DISABLE,
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = (1ULL << RELAY_GPIO),
        .pull_down_en = 0,
        .pull_up_en = 0
    };
    gpio_config(&io_conf);
    ESP_LOGI(TAG, "Relay initialized on GPIO %d", RELAY_GPIO);
}

void relay_check_state(void)
{
    int level = gpio_get_level(RELAY_GPIO);
    ESP_LOGI(TAG, "Relay state: %s", level ? "ON" : "OFF");
}
void relay_set(bool on)
{
    gpio_set_level(RELAY_GPIO, on ? 1 : 0);
    ESP_LOGI(TAG, "Relay turned %s", on ? "ON" : "OFF");
}

