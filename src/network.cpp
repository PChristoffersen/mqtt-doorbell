#include "network.h"

#include <stdio.h>
#include <string.h>
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/message_buffer.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "nvs_flash.h"

#include "mqtt.h"
#include "battery.h"

//#define CONFIGURE_WIFI

#ifdef CONFIGURE_WIFI
constexpr char DOORBELL_ESP_WIFI_SSID[] = "<WIFI_SSID>";
constexpr char DOORBELL_ESP_WIFI_PASS[] = "<WIFI_PASSWORD>";
#define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WPA2_PSK
#endif


constexpr uint DOORBELL_NETWORK_STARTUP_DELAY_MS { 1000 };
constexpr uint DOORBELL_ESP_MAXIMUM_RETRY { 2 };


static constexpr char TAG[] = "doorbell_net";

static wifi_config_t g_wifi_config;
static TaskHandle_t g_network_task;

/* FreeRTOS event group to signal when we are connected*/
static EventGroupHandle_t g_wifi_event_group;
static MessageBufferHandle_t g_trigger_buffer;


/* The event group allows multiple bits for each event, but we only care about two events:
 * - we are connected to the AP with an IP
 * - we failed to connect after the maximum amount of retries */
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1
#define WIFI_SHUTDOWN_BIT  BIT2
#define WIFI_TERM_BIT      BIT3


enum Event {
    EVT_SHUTDOWN,
    EVT_TRIGGER_PRESS,
    EVT_TRIGGER_RELEASE,
};


static void event_handler(void* arg, esp_event_base_t event_base,
                                int32_t event_id, void* event_data)
{
    static int retry_num = 0;
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        ESP_LOGI(TAG,"Wifi STA start");
        esp_wifi_connect();
    }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_STOP) {
        ESP_LOGI(TAG,"Wifi STA stop");
        retry_num = 0;
    }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        auto bits = xEventGroupGetBits(g_wifi_event_group);
        if (!(bits & WIFI_SHUTDOWN_BIT)) {
            if (retry_num < DOORBELL_ESP_MAXIMUM_RETRY) {
                esp_wifi_connect();
                retry_num++;
                ESP_LOGI(TAG, "retry to connect to the AP");
            } else {
                xEventGroupSetBits(g_wifi_event_group, WIFI_FAIL_BIT);
            }
            ESP_LOGI(TAG,"connect to the AP fail");
        }
    } 
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "got ip:" IPSTR, IP2STR(&event->ip_info.ip));
        retry_num = 0;
        xEventGroupSetBits(g_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}


void wifi_init_sta(void)
{
    g_wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());

    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &event_handler,
                                                        NULL,
                                                        &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &event_handler,
                                                        NULL,
                                                        &instance_got_ip));

    memset(&g_wifi_config, 0x00, sizeof(g_wifi_config));
    #ifdef CONFIGURE_WIFI
    strcpy((char*)wifi_config.sta.ssid, DOORBELL_ESP_WIFI_SSID);
    strcpy((char*)wifi_config.sta.password, DOORBELL_ESP_WIFI_PASS);
    wifi_config.sta.threshold.authmode = ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD;
    wifi_config.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
    #endif

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA) );
    #ifdef CONFIGURE_WIFI
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &g_wifi_config));
    #else
    esp_wifi_get_config(WIFI_IF_STA, &g_wifi_config);
    #endif
    ESP_ERROR_CHECK(esp_wifi_start() );

    ESP_LOGI(TAG, "wifi_init_sta finished.");

}


static void network_task_func(void *param) {
    vTaskDelay(pdMS_TO_TICKS(DOORBELL_NETWORK_STARTUP_DELAY_MS));

    wifi_init_sta();

    EventBits_t state_bits;
    state_bits = xEventGroupWaitBits(g_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT, pdFALSE, pdFALSE, portMAX_DELAY);

    if (state_bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "connected to ap SSID:%s", g_wifi_config.sta.ssid);
        mqtt_init();
    } 
    else if (state_bits & WIFI_FAIL_BIT) {
        ESP_LOGI(TAG, "Failed to connect to SSID:%s", g_wifi_config.sta.ssid);
    } 
    else {
        ESP_LOGE(TAG, "UNEXPECTED EVENT");
    }


    printf("Wifi task running\n");
    bool running = true;
    while (running && (state_bits & WIFI_CONNECTED_BIT)) {
        Event evt = EVT_SHUTDOWN;
        size_t sz = xMessageBufferReceive(g_trigger_buffer, &evt, sizeof(evt), pdMS_TO_TICKS(1000));
        if (sz) {
            switch (evt) {
                case EVT_SHUTDOWN:
                    running = false;
                    break;
                case EVT_TRIGGER_PRESS:
                    mqtt_send_button(true);
                    break;
                case EVT_TRIGGER_RELEASE:
                    mqtt_send_button(false);
                    break;
            }

        }

        state_bits = xEventGroupGetBits(g_wifi_event_group);
    }

    printf("Wifi task shutting down\n");

    // Send battery status and stop wifi
    if (state_bits & WIFI_CONNECTED_BIT) {
        printf("Sending battery state\n");

        mqtt_send_battery(battery_read_voltage_mv());

        mqtt_term();
    }

    xEventGroupSetBits(g_wifi_event_group, WIFI_SHUTDOWN_BIT);
    esp_wifi_stop();
    xEventGroupSetBits(g_wifi_event_group, WIFI_TERM_BIT);

    while (true) {
        vTaskDelay(portMAX_DELAY);
    }
}


void network_init() {
    printf("Network init\n");

    g_trigger_buffer = xMessageBufferCreate(32);

    xTaskCreate(network_task_func, "network", 4*configMINIMAL_STACK_SIZE, nullptr, 1, &g_network_task);
}


void network_term() {
    printf("Network term\n");

    Event evt = EVT_SHUTDOWN;
    xMessageBufferSend(g_trigger_buffer, &evt, sizeof(evt), portMAX_DELAY);

    printf("Waiting for network task to complete\n");
    xEventGroupWaitBits(g_wifi_event_group, WIFI_TERM_BIT, pdFALSE, pdFALSE, portMAX_DELAY);

    printf("Network shutdown\n");

    vTaskDelete(g_network_task);
}



void network_notify_press(bool state) {
    printf("Notify: %s\n", state?"true":"false");
    Event evt = state ? EVT_TRIGGER_PRESS : EVT_TRIGGER_RELEASE;
    xMessageBufferSend(g_trigger_buffer, &evt, sizeof(evt), portMAX_DELAY);
}
