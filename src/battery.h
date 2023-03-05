#pragma once

#include <stdio.h>
#include "sdkconfig.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"

void battery_init();
uint battery_read_voltage_mv();
uint battery_to_percent(uint voltage_mv);
