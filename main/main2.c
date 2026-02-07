// Reliable Wi-Fi + DNS + SNTP + AWS MQTT + OTA (ESP-IDF v5.3+)

#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <inttypes.h>
#include <time.h>
#include <sys/param.h>
#include <sys/socket.h>
#include <netdb.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include "esp_system.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_timer.h"
#include "esp_sleep.h"
#include "esp_mac.h"
#include "esp_chip_info.h"

#include "nvs_flash.h"
#include "nvs.h"

#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_netif_ip_addr.h"

#include "esp_sntp.h"
#include "mdns.h"

#include "driver/gpio.h"

#include "esp_app_desc.h"

#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_ota_ops.h"

#include "mqtt_client.h"
#include "cJSON.h"

// --------------------------- CONFIG ---------------------------

// Toggle log detail
#define LOG_DEBUG   0     // set 1 for verbose debug, keep 0 for minimal

// Device + topics
#define DEVICE_ID         "ESP32_Device_1"
#define TOPIC_CMD         "devices/" DEVICE_ID "/command"
#define TOPIC_STATE       "devices/" DEVICE_ID "/state"
#define TOPIC_HEARTBEAT   "devices/" DEVICE_ID "/heartbeat"

// AWS IoT
#define AWS_ENDPOINT_URI  "mqtts://a11jfphiwx6u88-ats.iot.ap-south-1.amazonaws.com"

// OTA
#define OTA_MANIFEST_URL  "https://home-ai-bucket.s3.ap-south-1.amazonaws.com/firmware/manifest.json"
#define OTA_TASK_STACK    8192
#define OTA_CHECK_MIN     60        // periodic manifest check interval (minutes)
#define OTA_FIRST_DELAY_MS 5000

// Wi-Fi SoftAP fallback
#define AP_SSID           "MyDevice_Setup"
#define AP_PASSWORD       ""        // open AP
#define AP_CHANNEL        1
#define AP_MAX_CONN       4

// Wi-Fi STA retry policy
#define STA_TOTAL_WINDOW_MS   25000   // overall wait for IP before falling back
#define STA_RETRY_DELAY_MS     5000   // retry connect delay
#define MAX_STA_RETRIES           5

// Heartbeat
#define HEARTBEAT_MS        60000

// Rollback grace
#define ROLLBACK_GRACE_AFTER_MQTT_MS   15000

// --------------------------- CERTS (embedded) ---------------------------
// Match your filenames and CMake EMBED_TXTFILES
extern const uint8_t rootCA_pem_start[]         asm("_binary_rootCA_pem_start");
extern const uint8_t rootCA_pem_end[]           asm("_binary_rootCA_pem_end");
extern const uint8_t device_cert_pem_crt_start[] asm("_binary_device_cert_pem_crt_start");
extern const uint8_t device_cert_pem_crt_end[]   asm("_binary_device_cert_pem_crt_end");
extern const uint8_t device_key_pem_key_start[]  asm("_binary_device_key_pem_key_start");
extern const uint8_t device_key_pem_key_end[]    asm("_binary_device_key_pem_key_end");

// --------------------------- GLOBALS ---------------------------
static const char *TAG = "APP";

static EventGroupHandle_t s_wifi_evt;
static esp_mqtt_client_handle_t s_mqtt = NULL;
// static httpd_handle_t s_httpd = NULL;  // (kept for future; not used here)

static volatile bool g_mqtt_connected   = false;
static volatile uint32_t g_mqtt_disc    = 0;
static volatile uint32_t g_wifi_lost    = 0;

// Relay pins (safe set for WROOM)
static const int RELAYS[10] = {5,4,16,17,18,19,21,22,23,25};

// Bits
#define WIFI_GOT_IP_BIT  BIT0
#define WIFI_FAIL_BIT    BIT1

// --------------------------- UTILS ---------------------------
static inline void relays_init(void) {
    for (int i=0;i<10;i++) {
        gpio_reset_pin(RELAYS[i]);
        gpio_set_direction(RELAYS[i], GPIO_MODE_OUTPUT);
        gpio_set_level(RELAYS[i], 0);
    }
}
static inline void relays_bits_str(char out10[11]){
    for (int i=0;i<10;i++) out10[i] = gpio_get_level(RELAYS[i]) ? '1':'0';
    out10[10]=0;
}
static inline void relay_set_1based(int idx, int val){
    if (idx<1 || idx>10) return;
    gpio_set_level(RELAYS[idx-1], val?1:0);
}
static void format_mac(const uint8_t mac[6], char out[18]) {
    snprintf(out,18,"%02X:%02X:%02X:%02X:%02X:%02X",mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);
}
static void ip4_to_str(const esp_ip4_addr_t *a, char out[16]) {
    if (!a) { strcpy(out,"0.0.0.0"); return; }
    snprintf(out,16,"%u.%u.%u.%u", esp_ip4_addr1(a), esp_ip4_addr2(a),
                                  esp_ip4_addr3(a), esp_ip4_addr4(a));
}
static uint32_t chip_id32(void){
    uint8_t m[6]; esp_read_mac(m, ESP_MAC_WIFI_STA);
    return ((uint32_t)m[3]<<16) | ((uint32_t)m[4]<<8) | m[5];
}
static void iso8601_utc(char out[25]){
    struct timeval tv; gettimeofday(&tv,NULL);
    time_t now = tv.tv_sec; struct tm tm;
    gmtime_r(&now,&tm);
    strftime(out,25,"%Y-%m-%dT%H:%M:%SZ",&tm);
}

// --------------------------- NVS Wi-Fi creds ---------------------------
// static void nvs_wifi_save(const char* ssid, const char* pass){
//     nvs_handle_t h; ESP_ERROR_CHECK(nvs_open("wifi", NVS_READWRITE, &h));
//     nvs_set_str(h,"ssid",ssid);
//     nvs_set_str(h,"pass",pass);
//     nvs_commit(h); nvs_close(h);
// }
static bool nvs_wifi_load(char* ssid, size_t ssid_len, char* pass, size_t pass_len){
    nvs_handle_t h; if (nvs_open("wifi", NVS_READONLY, &h)!=ESP_OK) return false;
    esp_err_t e1 = nvs_get_str(h,"ssid",ssid,&ssid_len);
    esp_err_t e2 = nvs_get_str(h,"pass",pass,&pass_len);
    nvs_close(h);
    return (e1==ESP_OK && e2==ESP_OK && ssid[0]);
}

// --------------------------- Wi-Fi Events ---------------------------
static void wifi_event_handler(void* arg, esp_event_base_t base, int32_t id, void* data){
    if (base==WIFI_EVENT) {
        switch (id){
            case WIFI_EVENT_STA_START:
                xEventGroupClearBits(s_wifi_evt, WIFI_GOT_IP_BIT|WIFI_FAIL_BIT);
                esp_wifi_connect();
                break;
            case WIFI_EVENT_STA_DISCONNECTED:
                g_wifi_lost++;
                xEventGroupSetBits(s_wifi_evt, WIFI_FAIL_BIT);
                xEventGroupClearBits(s_wifi_evt, WIFI_GOT_IP_BIT);
                #if LOG_DEBUG
                    ESP_LOGW(TAG, "Wi-Fi lost; reconnecting (count=%" PRIu32 ")", g_wifi_lost);
                #endif
                break;
            default: break;
        }
    } else if (base==IP_EVENT && id==IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* e = (ip_event_got_ip_t*)data;
        ESP_LOGI(TAG, "STA IP: " IPSTR, IP2STR(&e->ip_info.ip));
        xEventGroupSetBits(s_wifi_evt, WIFI_GOT_IP_BIT);
    }
}

static void softap_start(void){
    esp_netif_create_default_wifi_ap();
    wifi_config_t ap = {0};
    strncpy((char*)ap.ap.ssid, AP_SSID, sizeof(ap.ap.ssid)-1);
    ap.ap.ssid_len = strlen((char*)ap.ap.ssid);
    ap.ap.channel = AP_CHANNEL;
    ap.ap.max_connection = AP_MAX_CONN;
    ap.ap.authmode = WIFI_AUTH_OPEN; // per your selection
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));
    ESP_ERROR_CHECK(esp_wifi_start());
    mdns_init(); mdns_hostname_set("esp32-setup"); mdns_instance_name_set("ESP32 Setup");
    ESP_LOGW(TAG, "SoftAP started: %s", AP_SSID);
}

// One bounded STA attempt window, then AP
static bool sta_connect_window(uint32_t window_ms){
    esp_netif_create_default_wifi_sta();

    char ssid[64]={0}, pass[64]={0};
    if (!nvs_wifi_load(ssid,sizeof(ssid),pass,sizeof(pass))) {
        ESP_LOGW(TAG, "No saved Wi-Fi creds");
        return false;
    }

    wifi_config_t st = {0};
    strncpy((char*)st.sta.ssid, ssid, sizeof(st.sta.ssid)-1);
    strncpy((char*)st.sta.password, pass, sizeof(st.sta.password)-1);
    st.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    st.sta.pmf_cfg.capable = true;
    st.sta.pmf_cfg.required = false;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &st));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "Connecting SSID='%s'…", ssid);

    int retries = 0;
    int64_t deadline = esp_timer_get_time()/1000 + window_ms;

    while ((esp_timer_get_time()/1000) < deadline) {
        EventBits_t bits = xEventGroupWaitBits(s_wifi_evt, WIFI_GOT_IP_BIT, pdFALSE, pdFALSE, pdMS_TO_TICKS(2000));
        if (bits & WIFI_GOT_IP_BIT) return true;

        // retry every STA_RETRY_DELAY_MS
        if (++retries % (STA_RETRY_DELAY_MS/2000) == 0) {
            esp_wifi_disconnect();
            vTaskDelay(pdMS_TO_TICKS(200));
            esp_wifi_connect();
        }
    }
    return false;
}

// --------------------------- SNTP ---------------------------
static bool time_is_valid(void){
    time_t now = 0; struct tm tm={0};
    time(&now); localtime_r(&now,&tm);
    return (tm.tm_year >= (2016-1900));
}
static void sntp_sync(void){
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_setservername(1, "time.google.com");
    esp_sntp_setservername(2, "time.cloudflare.com");
    esp_sntp_init();

    for (int i=0;i<15;i++){
        if (time_is_valid()){
            ESP_LOGI(TAG, "Time synced");
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    ESP_LOGW(TAG, "SNTP timeout; continuing");
}

// --------------------------- DNS guard ---------------------------
static bool dns_can_resolve(const char* host, int timeout_ms){
    // lwIP getaddrinfo respects DNS; add a soft timeout loop
    int step = 200; int waited=0;
    while (waited < timeout_ms){
        struct addrinfo hints = {0}, *res = NULL;
        int rc = getaddrinfo(host, NULL, &hints, &res);
        if (rc==0 && res){ freeaddrinfo(res); return true; }
        vTaskDelay(pdMS_TO_TICKS(step));
        waited += step;
    }
    return false;
}

// --------------------------- MQTT ---------------------------
static void mqtt_publish_state(void){
    if (!s_mqtt) return;
    char rel[11]; relays_bits_str(rel);
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root,"deviceId", DEVICE_ID);
    cJSON_AddStringToObject(root,"relays", rel);
    char *js = cJSON_PrintUnformatted(root);
    esp_mqtt_client_publish(s_mqtt, TOPIC_STATE, js, 0, 1, 0);
    cJSON_free(js);
    cJSON_Delete(root);
}
static void mqtt_publish_heartbeat(void){
    if (!s_mqtt) return;
    char ts[25]; iso8601_utc(ts);
    uint8_t macb[6]; esp_read_mac(macb, ESP_MAC_WIFI_STA); char mac[18]; format_mac(macb, mac);
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    char ip[16]="0.0.0.0";
    if (netif){ esp_netif_ip_info_t info; if (esp_netif_get_ip_info(netif,&info)==ESP_OK) ip4_to_str(&info.ip, ip); }
    char rel[11]; relays_bits_str(rel);
    uint32_t up = (uint32_t)(esp_timer_get_time()/1000000ULL);

    const esp_app_desc_t* ad = esp_app_get_description();

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root,"deviceId", DEVICE_ID);
    cJSON_AddStringToObject(root,"timestamp", ts);
    cJSON_AddStringToObject(root,"ip", ip);
    cJSON_AddStringToObject(root,"firmware", ad->version);
    cJSON_AddNumberToObject(root,"uptime", up);
    cJSON_AddNumberToObject(root,"chipId", chip_id32());
    cJSON_AddStringToObject(root,"relays", rel);
    cJSON_AddStringToObject(root,"status", g_mqtt_connected ? "OK":"DISCONNECTED");
    char *js = cJSON_PrintUnformatted(root);
    esp_mqtt_client_publish(s_mqtt, TOPIC_HEARTBEAT, js, 0, 1, 0);
    cJSON_free(js); cJSON_Delete(root);
}
static void mqtt_evt(void *arg, esp_event_base_t base, int32_t eid, void *data){
    esp_mqtt_event_handle_t ev = (esp_mqtt_event_handle_t)data;
    switch (ev->event_id){
        case MQTT_EVENT_CONNECTED:
            g_mqtt_connected = true;
            ESP_LOGI(TAG, "MQTT connected");
            esp_mqtt_client_subscribe(s_mqtt, TOPIC_CMD, 1);
            mqtt_publish_state();
            break;
        case MQTT_EVENT_DISCONNECTED:
            g_mqtt_connected = false; g_mqtt_disc++;
            ESP_LOGW(TAG, "MQTT disconnected");
            break;
        case MQTT_EVENT_DATA: {
            if (ev->topic_len > 0 && strncmp(ev->topic, TOPIC_CMD, ev->topic_len)==0) {
                cJSON *cmd = cJSON_ParseWithLength(ev->data, ev->data_len);
                if (cmd){
                    cJSON *r=cJSON_GetObjectItem(cmd,"relay");
                    cJSON *s=cJSON_GetObjectItem(cmd,"state");
                    if (cJSON_IsNumber(r) && cJSON_IsNumber(s)){
                        relay_set_1based(r->valueint, s->valueint?1:0);
                        mqtt_publish_state();
                    }
                    cJSON_Delete(cmd);
                }
            }
            break;
        }
        case MQTT_EVENT_ERROR:
            ESP_LOGE(TAG, "MQTT error: type=%d", ev->error_handle? ev->error_handle->error_type:-1);
            break;
        default: break;
    }
}
static void mqtt_start(void){
    esp_mqtt_client_config_t cfg = {
        .broker = {
            .address.uri = AWS_ENDPOINT_URI,
            .verification = { .certificate = (const char*)rootCA_pem_start },
        },
        .credentials = {
            .client_id = DEVICE_ID,
            .authentication = {
                .certificate = (const char*)device_cert_pem_crt_start,
                .key         = (const char*)device_key_pem_key_start,
            },
        },
        .session = { .keepalive = 60 },
    };
    s_mqtt = esp_mqtt_client_init(&cfg);
    esp_mqtt_client_register_event(s_mqtt, ESP_EVENT_ANY_ID, mqtt_evt, NULL);
    esp_mqtt_client_start(s_mqtt);
}

// --------------------------- OTA ---------------------------
static int cmp_semver(const char *a, const char *b){
    int A1=0,A2=0,A3=0,B1=0,B2=0,B3=0;
    sscanf(a,"%d.%d.%d",&A1,&A2,&A3);
    sscanf(b,"%d.%d.%d",&B1,&B2,&B3);
    if (A1!=B1) return (A1<B1)?-1:1;
    if (A2!=B2) return (A2<B2)?-1:1;
    if (A3!=B3) return (A3<B3)?-1:1;
    return 0;
}
static esp_err_t http_get(const char* url, char* out, size_t outlen){
    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = 15000,
        .keep_alive_enable = true,
        .user_agent = "esp32-ota",
        .cert_pem = (const char*)rootCA_pem_start,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) return ESP_FAIL;
    esp_err_t err = esp_http_client_open(c,0);
    if (err!=ESP_OK){ esp_http_client_cleanup(c); return err; }
    int code = esp_http_client_fetch_headers(c);
    (void)code;
    int total=0, r=0;
    while ((r=esp_http_client_read(c, out+total, outlen-total-1))>0){
        total+=r; if (total>=(int)outlen-1) break;
    }
    out[total]=0;
    esp_http_client_close(c); esp_http_client_cleanup(c);
    return (total>0)?ESP_OK:ESP_FAIL;
}
static esp_err_t do_ota(const char* url){
    esp_https_ota_config_t ocfg = {
        .http_config = &(esp_http_client_config_t){
            .url = url,
            .timeout_ms = 15000,
            .keep_alive_enable = true,
            .user_agent = "esp32-ota",
            .cert_pem = (const char*)rootCA_pem_start,
        },
        .partial_http_download = false,
    };
    ESP_LOGI(TAG, "OTA: starting: %s", url);
    esp_err_t r = esp_https_ota(&ocfg);
    if (r==ESP_OK){
        ESP_LOGI(TAG, "OTA: success, rebooting");
        if (s_mqtt){ esp_mqtt_client_stop(s_mqtt); esp_mqtt_client_destroy(s_mqtt); s_mqtt=NULL; }
        esp_wifi_disconnect();
        vTaskDelay(pdMS_TO_TICKS(200));
        esp_restart();
    } else {
        ESP_LOGE(TAG, "OTA: failed: %s", esp_err_to_name(r));
    }
    return r;
}
static void ota_task(void* arg){
    vTaskDelay(pdMS_TO_TICKS(OTA_FIRST_DELAY_MS));
    while (1){
        char json[1024]={0};
        if (http_get(OTA_MANIFEST_URL, json, sizeof(json))==ESP_OK){
            #if LOG_DEBUG
                        ESP_LOGI(TAG, "OTA: manifest %s", json);
            #endif
            cJSON* root = cJSON_Parse(json);
            if (root){
                const cJSON* v = cJSON_GetObjectItem(root,"version");
                const cJSON* u = cJSON_GetObjectItem(root,"url");
                if (cJSON_IsString(v) && cJSON_IsString(u)){
                    const esp_app_desc_t* ad = esp_app_get_description();
                    ESP_LOGI(TAG, "OTA: current=%s, new=%s", ad->version, v->valuestring);
                    if (cmp_semver(ad->version, v->valuestring) < 0){
                        ESP_LOGI(TAG, "OTA: newer available, starting");
                        do_ota(u->valuestring);
                    } else {
#if LOG_DEBUG
                        ESP_LOGI(TAG, "OTA: already up-to-date");
#endif
                    }
                }
                cJSON_Delete(root);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(OTA_CHECK_MIN*60000));
    }
}

// --------------------------- Rollback VALID mark ---------------------------
static void rollback_mark_task(void* arg){
    int tries=100;
    while (tries-- > 0 && !g_mqtt_connected) vTaskDelay(pdMS_TO_TICKS(100));
    if (!g_mqtt_connected){ vTaskDelete(NULL); return; }
    vTaskDelay(pdMS_TO_TICKS(ROLLBACK_GRACE_AFTER_MQTT_MS));
    const esp_partition_t* runp = esp_ota_get_running_partition();
    esp_ota_img_states_t st;
    if (esp_ota_get_state_partition(runp,&st)==ESP_OK && st==ESP_OTA_IMG_PENDING_VERIFY){
        if (esp_ota_mark_app_valid_cancel_rollback()==ESP_OK)
            ESP_LOGI(TAG, "Marked app VALID");
        else
            ESP_LOGE(TAG, "Failed to mark app valid");
    }
    vTaskDelete(NULL);
}

// --------------------------- app_main ---------------------------
void app_main(void)
{
    // Minimal logs unless LOG_DEBUG=1
#if LOG_DEBUG
    esp_log_level_set("*", ESP_LOG_INFO);
#else
    esp_log_level_set("*", ESP_LOG_WARN);
    esp_log_level_set("APP", ESP_LOG_INFO);
    esp_log_level_set("wifi", ESP_LOG_INFO);
#endif

    // Version (from IDF descriptor)
    const esp_app_desc_t* ad = esp_app_get_description();
    ESP_LOGI(TAG, "FW %s", ad->version);

    // Core init
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    s_wifi_evt = xEventGroupCreate();

    wifi_init_config_t wcfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wcfg));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL, NULL));

    relays_init();

    // Try STA within window, else AP
    if (!sta_connect_window(STA_TOTAL_WINDOW_MS)) {
        ESP_LOGW(TAG, "STA failed within window -> SoftAP");
        softap_start();
        return; // wait for user provisioning flow (add later if needed)
    }

    // DNS guard: wait until AWS host resolves
    // strip scheme from URI for getaddrinfo
    const char* host = "a11jfphiwx6u88-ats.iot.ap-south-1.amazonaws.com";
    if (!dns_can_resolve(host, 15000)) {
        ESP_LOGW(TAG, "DNS slow for %s; continuing", host);
    }

    // SNTP
    sntp_sync();

    // mDNS optional on STA
    mdns_init(); mdns_hostname_set("esp32-device"); mdns_instance_name_set("ESP32 Device");

    // Start MQTT (network stable now)
    mqtt_start();

    // OTA checker
    xTaskCreatePinnedToCore(ota_task, "ota_check", OTA_TASK_STACK, NULL, 4, NULL, tskNO_AFFINITY);

    // Rollback VALID after MQTT OK
    const esp_partition_t* runp = esp_ota_get_running_partition();
    esp_ota_img_states_t st;
    if (esp_ota_get_state_partition(runp,&st)==ESP_OK && st==ESP_OTA_IMG_PENDING_VERIFY) {
        ESP_LOGW(TAG, "Image PENDING_VERIFY; will mark VALID after MQTT");
        xTaskCreatePinnedToCore(rollback_mark_task, "rb_mark", 3072, NULL, 5, NULL, tskNO_AFFINITY);
    }

    // Heartbeat loop
    int64_t last = esp_timer_get_time();
    for(;;){
        int64_t now = esp_timer_get_time();
        if ((now-last)/1000 >= HEARTBEAT_MS) {
            mqtt_publish_heartbeat();
            last = now;
        }
        vTaskDelay(pdMS_TO_TICKS(200));
    }
}

