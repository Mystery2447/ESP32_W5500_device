#pragma once

#include <stdbool.h>
#include "driver/gpio.h"

#define RELAY1_GPIO  GPIO_NUM_15
#define RELAY2_GPIO  GPIO_NUM_13

void relay_init(void);               /* 初始化两路继电器 */
void relay_set(int num, bool on);    /* num: 1 或 2 */
void relay_check_state(int num);