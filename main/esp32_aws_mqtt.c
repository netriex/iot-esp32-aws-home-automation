#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "mqtt_client.h"
#include "driver/gpio.h"

#include "device_cert.h"      // Your device certificate
#include "device_private_key.h" // Your private key
#include "aws_root_ca.h"       // AWS Root CA certificate

#include "cJSON.h"

// WiFi credentials
#define WIFI_SSID      "YourSSID"
#define WIFI_PASS      "YourPassword"

// MQTT broker info
#define MQTT_BROKER_URI "mqtts://abcdefg.iot.ap-south-1.amazonaws.com:8883"
#define MQTT_DEVICE_ID  "esp32_b84"   // Unique device ID
#define MQTT_TOPIC_RELAY "home/esp32/relay"
#define MQTT_TOPIC_STATE "home/esp32/state"

// Relay GPIOs
#define RELAY1_PIN 5
#define RELAY2_PIN 18
#define RELAY3_PIN 19

static const char *TAG = "ESP32_AWS";

// MQTT client handle
esp_mqtt_client_handle_t client;

// Last known relay states
static int last_state1 = -1;
static int last_state2 = -1;
static int last_state3 = -1;

// Initialize relays
void init_relays() {
    gpio_reset_pin(RELAY1_PIN);
    gpio_set_direction(RELAY1_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(RELAY1_PIN, 0);

    gpio_reset_pin(RELAY2_PIN);
    gpio_set_direction(RELAY2_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(RELAY2_PIN, 0);
    
    gpio_reset_pin(RELAY3_PIN);
    gpio_set_direction(RELAY3_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(RELAY3_PIN, 0);
}

// WiFi initialization (Station mode)
void wifi_init_sta(void) {
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASS,
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
            .pmf_cfg = {
                .capable = true,
                .required = false
            },
        },
    };

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_connect());

    ESP_LOGI(TAG, "WiFi connecting to SSID: %s", WIFI_SSID);
}

// MQTT Event Handler
static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    esp_mqtt_event_handle_t event = event_data;

    switch (event->event_id)
    {
        case MQTT_EVENT_CONNECTED:
            ESP_LOGI(TAG, "MQTT Connected");
            esp_mqtt_client_subscribe(client, MQTT_TOPIC_RELAY, 1);
            break;

        case MQTT_EVENT_DISCONNECTED:
            ESP_LOGW(TAG, "MQTT Disconnected");
            break;

        case MQTT_EVENT_DATA:
            ESP_LOGI(TAG, "Received data: topic=%.*s", event->topic_len, event->topic);

            cJSON *json = cJSON_ParseWithLength(event->data, event->data_len);
            if (json) {
                cJSON *item;
                item = cJSON_GetObjectItem(json, "switch1");
                if (item) gpio_set_level(RELAY1_PIN, item->valueint);

                item = cJSON_GetObjectItem(json, "switch2");
                if (item) gpio_set_level(RELAY2_PIN, item->valueint);

                item = cJSON_GetObjectItem(json, "switch3");
                if (item) gpio_set_level(RELAY3_PIN, item->valueint);

                cJSON_Delete(json);
            }
            break;

        case MQTT_EVENT_ERROR:
            ESP_LOGE(TAG, "MQTT Error occurred");
            break;

        default:
            break;
    }
}

// MQTT initialization with TLS & reconnect
void mqtt_app_start(void)
{
    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = MQTT_BROKER_URI,
        .credentials = {
            .client_cert_pem = (const char *)certificate_pem_crt,
            .client_key_pem  = (const char *)private_pem_key,
            .cert_pem        = (const char *)aws_root_ca_pem,
        },
        .network.transport = MQTT_TRANSPORT_OVER_SSL,
        .client_id = MQTT_DEVICE_ID,
        .disable_auto_reconnect = false,
        .keepalive = 120,
    };

    client = esp_mqtt_client_init(&mqtt_cfg);
    ESP_ERROR_CHECK(esp_mqtt_client_register_event(client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL));
    ESP_ERROR_CHECK(esp_mqtt_client_start(client));
}

// Publish relay state only when changed
void publish_state_if_changed() {
    int s1 = gpio_get_level(RELAY1_PIN);
    int s2 = gpio_get_level(RELAY2_PIN);
    int s3 = gpio_get_level(RELAY3_PIN);

    if (s1 != last_state1 || s2 != last_state2 || s3 != last_state3) {
        char payload[128];
        sprintf(payload, "{\"switch1\":%d,\"switch2\":%d,\"switch3\":%d}", s1, s2, s3);
        esp_mqtt_client_publish(client, MQTT_TOPIC_STATE, payload, 0, 1, 0);

        last_state1 = s1;
        last_state2 = s2;
        last_state3 = s3;

        ESP_LOGI(TAG, "Published relay state: %s", payload);
    }
}

// Main application
void app_main(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());

    init_relays();
    wifi_init_sta();

    mqtt_app_start();

    while (1) {
        publish_state_if_changed();
        vTaskDelay(pdMS_TO_TICKS(1000)); // check every 1 second
    }
}
