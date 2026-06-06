#pragma once

#include <stdbool.h>

void app_wifi_init(void);
bool app_wifi_is_connected(void);
void app_wifi_wait_connected(void);
