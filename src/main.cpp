#include <stdio.h>
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_sleep.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "driver/gpio.h"

#include "battery.h"
#include "network.h"


extern "C" {
    void app_main(void);
}


static constexpr bool ENABLE_SLEEP { true };


static constexpr gpio_num_t BUTTON_PIN { GPIO_NUM_2 };
static constexpr gpio_num_t RELAY_PIN { GPIO_NUM_3 };


static constexpr uint DINGDONG_MIN_COUNT { 3 };
static constexpr uint DINGDONG_HIGH_MS   { 300 };
static constexpr uint DINGDONG_LOW_MS    { 300 };

static constexpr uint64_t US_PER_SEC { 1000000llu };
static constexpr uint64_t US_PER_MIN { 60ull * US_PER_SEC };
static constexpr uint64_t SLEEP_DURATION_US { 60ull*US_PER_MIN };

static constexpr TickType_t AWAKE_DURATION_LONG_MS  { 15000 };
static constexpr TickType_t AWAKE_DURATION_SHORT_MS {  1000 };

static constexpr char TAG[] = "doorbell";


static void dingdong() {
    gpio_set_level(RELAY_PIN, 1);
    vTaskDelay(pdMS_TO_TICKS(DINGDONG_HIGH_MS));

    gpio_set_level(RELAY_PIN, 0);
    vTaskDelay(pdMS_TO_TICKS(DINGDONG_LOW_MS));
}


static void trigger_dingdong() {
    // Button pressed
    printf("Button pressed\n");
    network_notify_press(true);

    for (uint i=0; i<DINGDONG_MIN_COUNT; i++) {
        printf("%u dingdong\n", i);
        dingdong();
    }
    while (gpio_get_level(BUTTON_PIN)==0) {
        printf("* dingdong\n");
        dingdong();
    }
    printf("Button released\n");
    network_notify_press(false);
}



static void enter_sleep() {
    printf("Sleeeping\n");
    fflush(stdout);
    if constexpr (ENABLE_SLEEP) {
        esp_deep_sleep_start();
        printf("Sleep failed\n");
    }
}




static void app_init() {
    // Configure button pin
    gpio_config_t io_conf;
    io_conf.pin_bit_mask = (1ULL << BUTTON_PIN);
    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.pull_up_en = GPIO_PULLUP_ENABLE;
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io_conf.intr_type = GPIO_INTR_LOW_LEVEL;
    gpio_config(&io_conf);
    gpio_hold_en(BUTTON_PIN);
    gpio_wakeup_enable(BUTTON_PIN,  GPIO_INTR_LOW_LEVEL);

    // Configure relay pin
    io_conf.pin_bit_mask = (1ULL << RELAY_PIN);
    io_conf.mode = GPIO_MODE_OUTPUT;
    io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io_conf.intr_type = GPIO_INTR_DISABLE;
    gpio_config(&io_conf);
    gpio_set_level(RELAY_PIN, 0);

    battery_init();

    //Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
      ESP_ERROR_CHECK(nvs_flash_erase());
      ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Configure sleep modes
    esp_sleep_enable_timer_wakeup(SLEEP_DURATION_US);
    esp_sleep_enable_gpio_wakeup();
    esp_deep_sleep_enable_gpio_wakeup(1ULL<<BUTTON_PIN, ESP_GPIO_WAKEUP_GPIO_LOW);

}



void app_main(void)
{
    app_init();

    vTaskPrioritySet(nullptr, 2);
    network_init();

    auto voltage = battery_read_voltage_mv();
    printf("Battery voltage %u mV\n", voltage);


    TickType_t awake_duration = pdMS_TO_TICKS(AWAKE_DURATION_LONG_MS);

    switch (esp_sleep_get_wakeup_cause()) {
        case ESP_SLEEP_WAKEUP_TIMER:
            printf("Woken by TIMER\n");
            awake_duration = pdMS_TO_TICKS(AWAKE_DURATION_SHORT_MS);
            break;
        case ESP_SLEEP_WAKEUP_GPIO:
            printf("Woken by GPIO\n");
            trigger_dingdong();
            break;
        default:
            printf("Woken by other %d\n", esp_sleep_get_wakeup_cause());
            break;
    }

    if constexpr (!ENABLE_SLEEP) {
        awake_duration = pdMS_TO_TICKS(60000);
    }

    auto last_trigger = xTaskGetTickCount();

    printf("Entering loop sleep_time: %lu\n", awake_duration);
    while (true) {
        if (gpio_get_level(BUTTON_PIN)==0) {
            trigger_dingdong();
            last_trigger = xTaskGetTickCount();
        }

        vTaskDelay(pdMS_TO_TICKS(10));
        if ((xTaskGetTickCount()-last_trigger)>awake_duration) {
            printf("Idle!\n");
            break;
        }
    }
    printf("Exit loop\n");


    network_term();
    enter_sleep();

    // Sleep failed!
    esp_restart();
}

