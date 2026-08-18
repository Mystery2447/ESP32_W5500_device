#include "relay.h"
#include "driver/gpio.h"
#include "esp_log.h"

static const char *TAG = "RELAY";

static const gpio_num_t relay_gpio[] = {
    RELAY1_GPIO,
    RELAY2_GPIO,
};

void relay_init(void)
{
    for (int i = 0; i < 2; i++) {
        gpio_config_t io_conf = {
            .intr_type    = GPIO_INTR_DISABLE,
            .mode         = GPIO_MODE_OUTPUT,
            .pin_bit_mask = (1ULL << relay_gpio[i]),
            .pull_down_en = 0,
            .pull_up_en   = 0
        };
        gpio_config(&io_conf);
        gpio_set_level(relay_gpio[i], 0);   /* 默认关闭 */
        ESP_LOGI(TAG, "Relay%d initialized on GPIO %d", i + 1, relay_gpio[i]);
    }
}

void relay_set(int num, bool on)
{
    if (num < 1 || num > 2) {
        ESP_LOGE(TAG, "Invalid relay number: %d", num);
        return;
    }
    gpio_set_level(relay_gpio[num - 1], on ? 1 : 0);
    ESP_LOGI(TAG, "Relay%d turned %s", num, on ? "ON" : "OFF");
}

void relay_check_state(int num)
{
    if (num < 1 || num > 2) {
        ESP_LOGE(TAG, "Invalid relay number: %d", num);
        return;
    }
    int level = gpio_get_level(relay_gpio[num - 1]);
    ESP_LOGI(TAG, "Relay%d state: %s", num, level ? "ON" : "OFF");
}