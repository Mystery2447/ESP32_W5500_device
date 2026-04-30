#pragma once

#include <stdbool.h>
#include "driver/gpio.h"

#define RELAY_GPIO  GPIO_NUM_21

void relay_init(void);
void relay_check_state(void);
void relay_set(bool on);
