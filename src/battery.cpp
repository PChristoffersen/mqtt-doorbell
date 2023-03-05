#include "battery.h"


static constexpr adc_channel_t BATTERY_ADC_CHANNEL { ADC_CHANNEL_4 };
static constexpr adc_atten_t   BATTERY_ADC_ATTEN { ADC_ATTEN_DB_11 };
static constexpr uint BATTERY_SAMPLE_COUNT  { 16 };
static constexpr uint BATTERY_ADC_R1 { 202500 }; // 182 gnd
static constexpr uint BATTERY_ADC_R2 { 199000 }; // 202+
static adc_oneshot_unit_handle_t battery_adc_handle;
static adc_cali_handle_t battery_cali_handle;





void battery_init() {
    // Configure ADC
    adc_oneshot_unit_init_cfg_t init_config1 = {
        .unit_id = ADC_UNIT_1,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };
    adc_oneshot_chan_cfg_t config = {
        .atten = BATTERY_ADC_ATTEN,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    adc_cali_curve_fitting_config_t cali_config = {
        .unit_id = ADC_UNIT_1,
        .atten = BATTERY_ADC_ATTEN,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    adc_oneshot_new_unit(&init_config1, &battery_adc_handle);
    adc_oneshot_config_channel(battery_adc_handle, BATTERY_ADC_CHANNEL, &config);
    adc_cali_create_scheme_curve_fitting(&cali_config, &battery_cali_handle);

}




uint battery_read_voltage_mv() {
    uint vbatt = 0;
    for (uint i=0; i<BATTERY_SAMPLE_COUNT; i++) {
        int raw = 0;
        int voltage = 0;
        adc_oneshot_read(battery_adc_handle, BATTERY_ADC_CHANNEL, &raw);
        adc_cali_raw_to_voltage(battery_cali_handle, raw, &voltage);
        vbatt += voltage;
    }
    vbatt /= BATTERY_SAMPLE_COUNT;

    return vbatt * (BATTERY_ADC_R1+BATTERY_ADC_R2) / BATTERY_ADC_R2;
}


uint battery_to_percent(uint voltage_mv) {
    if (voltage_mv>4200) 
        return 100;
    if (voltage_mv>4150)
        return 95;
    if (voltage_mv>4110)
        return 90;
    if (voltage_mv>4080)
        return 85;
    if (voltage_mv>4020)
        return 80;
    if (voltage_mv>3980)
        return 75;
    if (voltage_mv>3950)
        return 70;
    if (voltage_mv>3910)
        return 65;
    if (voltage_mv>3870)
        return 60;
    if (voltage_mv>3850)
        return 55;
    if (voltage_mv>3840)
        return 50;
    if (voltage_mv>3820)
        return 45;
    if (voltage_mv>3800)
        return 40;
    if (voltage_mv>3790)
        return 35;
    if (voltage_mv>3770)
        return 30;
    if (voltage_mv>3750)
        return 25;
    if (voltage_mv>3730)
        return 20;
    if (voltage_mv>3710)
        return 15;
    if (voltage_mv>3690)
        return 10;
    if (voltage_mv>3610)
        return 10;
    return 0;
}