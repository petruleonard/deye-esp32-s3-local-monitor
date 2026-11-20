#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t deye_modbus_init(void);
void deye_modbus_set_scan_paused(bool paused);

#ifdef __cplusplus
}
#endif

