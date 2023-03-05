#pragma once

#include <stdio.h>

void mqtt_init();

void mqtt_term();


void mqtt_send_button(bool state);
void mqtt_send_battery(uint voltage_mv);
