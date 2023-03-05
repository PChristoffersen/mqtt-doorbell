#include "mqtt.h"

#include <string.h>
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "mqtt_client.h"

#include "battery.h"

//#define CONFIGURE_MQTT

#ifdef CONFIGURE_MQTT
#error "Update values"
static constexpr char CFG_MQTT_USER[] = "homeassistant";
static constexpr char CFG_MQTT_PASSWORD[] = "<PASSWORD>";
static constexpr char CFG_MQTT_ADDRESS[] = "mqtt://<IP>";
#endif


static constexpr char TAG[] = "doorbell_mqtt";

#define MQTT_PREFIX "doorbell"
static constexpr char MQTT_BUTTON_TOPIC[] = MQTT_PREFIX "/button";
static constexpr char MQTT_BATTERY_VOLTAGE_TOPIC[] = MQTT_PREFIX "/battery_voltage";
static constexpr char MQTT_BATTERY_PERCENT_TOPIC[] = MQTT_PREFIX "/battery_percent";


static char MQTT_ADDRESS[64];
static char MQTT_USER[64];
static char MQTT_PASSWORD[128];

static char MQTT_CLIENT_ID[64];

static EventGroupHandle_t g_mqtt_event_group;

#define MQTT_CONNECTED_BIT      BIT0
#define MQTT_FAIL_BIT           BIT1


static esp_mqtt_client_handle_t g_client;


static void log_error_if_nonzero(const char *message, int error_code)
{
    if (error_code != 0) {
        ESP_LOGE(TAG, "Last error %s: 0x%x", message, error_code);
    }
}


/*
 * @brief Event handler registered to receive MQTT events
 *
 *  This function is called by the MQTT client event loop.
 *
 * @param handler_args user data registered to the event.
 * @param base Event base for the handler(always MQTT Base in this example).
 * @param event_id The id for the received event.
 * @param event_data The data for the event, esp_mqtt_event_handle_t.
 */
static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    ESP_LOGD(TAG, "Event dispatched from event loop base=%s, event_id=%ld", base, event_id);
    esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;
    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "MQTT_EVENT_CONNECTED");
        xEventGroupSetBits(g_mqtt_event_group, MQTT_CONNECTED_BIT);
        break;
    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "MQTT_EVENT_DISCONNECTED");
        xEventGroupClearBits(g_mqtt_event_group, MQTT_CONNECTED_BIT);
        break;

    case MQTT_EVENT_SUBSCRIBED:
        ESP_LOGI(TAG, "MQTT_EVENT_SUBSCRIBED, msg_id=%d", event->msg_id);
        break;
    case MQTT_EVENT_UNSUBSCRIBED:
        ESP_LOGI(TAG, "MQTT_EVENT_UNSUBSCRIBED, msg_id=%d", event->msg_id);
        break;
    case MQTT_EVENT_PUBLISHED:
        ESP_LOGI(TAG, "MQTT_EVENT_PUBLISHED, msg_id=%d", event->msg_id);
        break;
    case MQTT_EVENT_DATA:
        ESP_LOGI(TAG, "MQTT_EVENT_DATA");
        printf("TOPIC=%.*s\r\n", event->topic_len, event->topic);
        printf("DATA=%.*s\r\n", event->data_len, event->data);
        break;
    case MQTT_EVENT_ERROR:
        ESP_LOGI(TAG, "MQTT_EVENT_ERROR");
        if (event->error_handle->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT) {
            log_error_if_nonzero("reported from esp-tls", event->error_handle->esp_tls_last_esp_err);
            log_error_if_nonzero("reported from tls stack", event->error_handle->esp_tls_stack_err);
            log_error_if_nonzero("captured as transport's socket errno",  event->error_handle->esp_transport_sock_errno);
            ESP_LOGI(TAG, "Last errno string (%s)", strerror(event->error_handle->esp_transport_sock_errno));

        }
        xEventGroupSetBits(g_mqtt_event_group, MQTT_FAIL_BIT);
        break;
    default:
        ESP_LOGI(TAG, "Other event id:%d", event->event_id);
        break;
    }
}




void mqtt_init() {
    size_t sz;

    g_mqtt_event_group = xEventGroupCreate();

    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    sprintf(MQTT_CLIENT_ID, "doorbell_%02x%02X%02X%02x%02X%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    printf("MQTT ClientID: %s\n", MQTT_CLIENT_ID);

    nvs_handle handle;

    #ifdef CONFIGURE_MQTT
    ESP_ERROR_CHECK(nvs_open("mqtt", NVS_READWRITE, &handle));
    nvs_set_str(handle, "mqtt_address", CFG_MQTT_ADDRESS);
    nvs_set_str(handle, "mqtt_user", CFG_MQTT_USER);
    nvs_set_str(handle, "mqtt_password", CFG_MQTT_PASSWORD);
    nvs_commit(handle);
    nvs_close(handle);
    #endif


    esp_mqtt_client_config_t mqtt_cfg;
    memset(&mqtt_cfg, 0x00, sizeof(mqtt_cfg));
    mqtt_cfg.credentials.client_id = MQTT_CLIENT_ID;

    ESP_ERROR_CHECK(nvs_open("mqtt", NVS_READONLY, &handle));
    sz = sizeof(MQTT_ADDRESS);
    nvs_get_str(handle, "mqtt_address", MQTT_ADDRESS, &sz);
    sz = sizeof(MQTT_USER);
    nvs_get_str(handle, "mqtt_user", MQTT_USER, &sz);
    sz = sizeof(MQTT_PASSWORD);
    nvs_get_str(handle, "mqtt_password", MQTT_PASSWORD, &sz);
    nvs_close(handle);

    mqtt_cfg.broker.address.uri = MQTT_ADDRESS;
    mqtt_cfg.credentials.username = MQTT_USER;
    mqtt_cfg.credentials.authentication.password = MQTT_PASSWORD;

    
    g_client = esp_mqtt_client_init(&mqtt_cfg);
    esp_mqtt_client_register_event(g_client, (esp_mqtt_event_id_t)ESP_EVENT_ANY_ID, mqtt_event_handler, g_client);
    esp_mqtt_client_start(g_client);


    auto bits = xEventGroupWaitBits(g_mqtt_event_group, MQTT_CONNECTED_BIT | MQTT_FAIL_BIT, pdFALSE, pdFALSE, portMAX_DELAY);
    if (bits & MQTT_CONNECTED_BIT) {
        ESP_LOGI(TAG, "connected to MQTT");
    }
    else if (bits & MQTT_FAIL_BIT) {
        ESP_LOGI(TAG, "failed to connecto to MQTT");
    }
}


void mqtt_term() {
    TickType_t start = xTaskGetTickCount();
    auto bits = xEventGroupGetBits(g_mqtt_event_group);
    while ((bits & MQTT_CONNECTED_BIT) && !(bits & MQTT_FAIL_BIT) && esp_mqtt_client_get_outbox_size(g_client)) {
        vTaskDelay(pdTICKS_TO_MS(20));
        if ((xTaskGetTickCount()-start)>pdTICKS_TO_MS(500)) {
            break;
        }
    }

    esp_mqtt_client_stop(g_client);
}


void mqtt_send_button(bool state) {
    auto msg_id = esp_mqtt_client_publish(g_client, MQTT_BUTTON_TOPIC, state?"on":"off", 0, 1, 1);
    ESP_LOGI(TAG, "sent publish successful, msg_id=%d", msg_id);
}

void mqtt_send_battery(uint voltage_mv) {
    char buf[32];
    sprintf(buf, "%.2f", voltage_mv/1000.0);
    auto msg_id = esp_mqtt_client_publish(g_client, MQTT_BATTERY_VOLTAGE_TOPIC, buf, 0, 1, 1);
    ESP_LOGI(TAG, "sent publish successful, msg_id=%d", msg_id);

    sprintf(buf, "%u", battery_to_percent(voltage_mv));
    msg_id = esp_mqtt_client_publish(g_client, MQTT_BATTERY_PERCENT_TOPIC, buf, 0, 1, 1);
    ESP_LOGI(TAG, "sent publish successful, msg_id=%d", msg_id);
}
