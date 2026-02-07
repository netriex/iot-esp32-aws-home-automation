// Phase 4 — Robust Wi-Fi + SoftAP provisioning page + MQTT + LEDC dimmer/fan + OTA + rollback
// Commands supported on TOPIC_CMD:
//   {"relay":1,"state":1}
//   {"dimmer":0..100}
//   {"fan_speed":0..5}
//   {"action":"reset_wifi"}   // wipes saved SSID/PASS, reboots into SoftAP provisioning
//
// Topics (unchanged):
//   SUB: devices/ESP32_Device_1/command
//   PUB: devices/ESP32_Device_1/state
//   PUB: devices/ESP32_Device_1/heartbeat
//
// Auto version: esp_app_get_description()->version
// ESP-IDF v5.3+
//
// Pins:
//   Relays (10): 5,4,16,17,18,19,21,22,23,25   (active HIGH)
//   Dimmer PWM:  26  (LEDC, percent 0..100)
//   Fan PWM:     27  (LEDC, levels 0..5 -> 0..100%)

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <ctype.h>
#include <time.h>
#include <inttypes.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include "esp_system.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "nvs.h"

#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_sntp.h"
#include "esp_app_desc.h"
#include "esp_mac.h"
#include "esp_ota_ops.h"
#include "esp_https_ota.h"
#include "esp_http_client.h"

#include "mqtt_client.h"
#include "cJSON.h"

#include "driver/gpio.h"
#include "driver/ledc.h"

#include "esp_http_server.h"
#include "mdns.h"
#include "esp_task_wdt.h"

#include <lwip/netdb.h>          // getaddrinfo()
#include <sys/param.h> 

// ---- Forward Declarations / Stubs for Internal Helpers ----

// ✅ Wi-Fi helpers
bool wifi_is_connected(void);
void wifi_reconnect(void);

// ✅ MQTT helpers
bool mqtt_is_connected(void);
void mqtt_reconnect(void);

// ✅ OTA / System health stub
bool system_health_ok(void) {
    // In future, replace this with real CPU/memory/sensor health checks
    return true;
}


// ===== App config =====
#define DEVICE_ID      "ESP32_Device_1"  //"888_g_room_1_1_2"  // (Phone number_Floor_room-or-kitchen_room1-or-kitchen1_borad-number(which esp32)_relay-number)
#define AWS_ENDPOINT_HOST "a11jfphiwx6u88-ats.iot.ap-south-1.amazonaws.com"
#define AWS_ENDPOINT_URI  "mqtts://" AWS_ENDPOINT_HOST

#define TOPIC_CMD       "devices/" DEVICE_ID "/command"
#define TOPIC_STATE     "devices/" DEVICE_ID "/state"
#define TOPIC_HEARTBEAT "devices/" DEVICE_ID "/heartbeat"

#define HEARTBEAT_MS 60000

// OTA manifest (JSON with {"version":"x.y.z","url":"https://...bin"})
#define OTA_MANIFEST_URL  "https://home-ai-bucket.s3.ap-south-1.amazonaws.com/firmware/manifest.json"

// ===== Auto version (runtime, not hardcoded) =====
#define APP_VERSION (esp_app_get_description()->version)

// ===== Embedded certs (CMake EMBED_TXTFILES) =====
extern const uint8_t rootCA_pem_start[]            asm("_binary_rootCA_pem_start");
extern const uint8_t rootCA_pem_end[]              asm("_binary_rootCA_pem_end");
extern const uint8_t device_cert_pem_crt_start[]   asm("_binary_device_cert_pem_crt_start");
extern const uint8_t device_cert_pem_crt_end[]     asm("_binary_device_cert_pem_crt_end");
extern const uint8_t device_key_pem_key_start[]    asm("_binary_device_key_pem_key_start");
extern const uint8_t device_key_pem_key_end[]      asm("_binary_device_key_pem_key_end");

// ===== Logging =====
static const char *TAG = "APP";

// ===== Relays (10 outputs) =====
static const int RELAY_PINS[10] = {5,4,16,17,18,19,21,22,23,25};
static void dimmer_set_percent(int percent);
static void fan_set_level(int level);

// ===== LEDC pins =====
#define DIMMER_PIN   26
#define FAN_PWM_PIN  27

// ===== LEDC setup =====
#define LEDC_RES_BITS        LEDC_TIMER_8_BIT
#define LEDC_MAX_DUTY        ((1 << 8) - 1)

#define DIMMER_TIMER         LEDC_TIMER_0
#define DIMMER_MODE          LEDC_LOW_SPEED_MODE
#define DIMMER_FREQ_HZ       1000

#define FAN_TIMER            LEDC_TIMER_1
#define FAN_MODE             LEDC_LOW_SPEED_MODE
#define FAN_FREQ_HZ          25000

#define DIMMER_CHANNEL       LEDC_CHANNEL_0
#define FAN_CHANNEL          LEDC_CHANNEL_1

// ===== Wi-Fi bits =====
#define WIFI_CONNECTED_BIT BIT0

static EventGroupHandle_t s_wifi_eg = NULL;

// ===== State flags =====
static esp_mqtt_client_handle_t s_mqtt = NULL;
static volatile bool wifi_connected = false;
static volatile bool mqtt_connected = false;
// ---- Reconnect supervisor state ----
static int wifi_disconn_count = 0;
// static int mqtt_disconn_count = 0;
// static int softap_escalate_threshold = 12; // ~12 failed cycles before AP (tunable)
static TickType_t last_wifi_disconn_tick = 0;
static bool provisioning_mode = false;


// ===== Dimmer/Fan state =====
static int g_dimmer_percent = 0;  // 0..100
static int g_fan_level      = 0;  // 0..5

// ===== SoftAP web =====
static httpd_handle_t s_httpd = NULL;

// ===== Phase-4 robustness timing =====
#define DNS_RETRY_COUNT               5
#define DNS_RETRY_DELAY_MS            2000
#define OTA_FIRST_CHECK_DELAY_MS      30000   // wait 30s after boot before 1st OTA check
#define OTA_CHECK_PERIOD_MIN          60      // periodic check every 60 min
#define POST_OTA_HEALTH_TIMEOUT_SEC   120     // 2 min to get Wi-Fi + MQTT after new FW

#ifndef esp_mqtt_client_is_connected
// compatibility stub for ESP-IDF <5.4
static inline bool esp_mqtt_client_is_connected(esp_mqtt_client_handle_t client) {
    return mqtt_connected;
}
#endif

// ---- Local connectivity helper stubs (fix for linker errors) ----

// ✅ Return Wi-Fi connection state from your global flag
bool wifi_is_connected(void) {
    extern volatile bool wifi_connected;
    return wifi_connected;
}

// ✅ Attempt reconnection (wrapper for esp_wifi_connect)
void wifi_reconnect(void) {
    ESP_LOGW("WIFI", "Reconnecting Wi-Fi...");
    esp_wifi_connect();
}

// ✅ Return MQTT connection state from your global flag
bool mqtt_is_connected(void) {
    extern volatile bool mqtt_connected;
    return mqtt_connected;
}

// ✅ Attempt MQTT reconnect
void mqtt_reconnect(void) {
    extern esp_mqtt_client_handle_t s_mqtt;
    if (s_mqtt) {
        ESP_LOGW("MQTT", "Forcing MQTT reconnect...");
        esp_mqtt_client_reconnect(s_mqtt);
    } else {
        ESP_LOGW("MQTT", "No client handle, skipping reconnect.");
    }
}


// ---------- small utils ----------
static void relays_init(void){
    for (int i = 0; i < 10; i++){
        gpio_reset_pin(RELAY_PINS[i]);
        gpio_set_direction(RELAY_PINS[i], GPIO_MODE_OUTPUT);
        gpio_set_level(RELAY_PINS[i], 0);
    }
}
static inline void relay_set_1based(int idx1, int val){
    if (idx1 < 1 || idx1 > 10) return;
    gpio_set_level(RELAY_PINS[idx1 - 1], val ? 1 : 0);
}
static void relays_bits_str(char out10[11]){
    for (int i = 0; i < 10; i++){
        out10[i] = gpio_get_level(RELAY_PINS[i]) ? '1' : '0';
    }
    out10[10] = '\0';
}
static void get_iso8601_utc(char out[25]){
    struct timeval tv;
    gettimeofday(&tv, NULL);
    time_t now = tv.tv_sec;
    struct tm tm;
    gmtime_r(&now, &tm);
    strftime(out, 25, "%Y-%m-%dT%H:%M:%SZ", &tm);
}
static void url_decode(char *dst, const char *src){
    while (*src){
        if (*src == '%' && isxdigit((int)src[1]) && isxdigit((int)src[2])){
            int a = src[1] <= '9' ? src[1] - '0' : (toupper(src[1]) - 'A' + 10);
            int b = src[2] <= '9' ? src[2] - '0' : (toupper(src[2]) - 'A' + 10);
            *dst++ = (char)(a * 16 + b);
            src += 3;
        } else if (*src == '+'){
            *dst++ = ' ';
            src++;
        } else {
            *dst++ = *src++;
        }
    }
    *dst = 0;
}
// ---------- Multi-command JSON helpers (non-breaking) ----------
static bool _starts_with(const char *s, const char *p) {
    if (!s || !p) return false;
    size_t n = strlen(p);
    return strncmp(s, p, n) == 0;
}   
static bool _apply_floor_object(cJSON *floorObj) {
    bool changed = false;
    if (!cJSON_IsObject(floorObj)) return false;

    cJSON *it = NULL;
    cJSON_ArrayForEach(it, floorObj) {
        const char *k = it->string;
        if (!k) continue;

        // RelayN
        if (_starts_with(k, "relay")) {
            const char *num_str = k + 5;
            int idx1 = atoi(num_str);
            if (cJSON_IsNumber(it)) {
                int val = it->valueint ? 1 : 0;
                ESP_LOGI(TAG, "Applying key=%s, value=%d", k, it->valueint);
                relay_set_1based(idx1, val);
                ESP_LOGI(TAG, "Setting relay %d = %d", idx1, val);
                changed = true;
            }
            continue;
        }

        // Brightness
        if (strcmp(k, "brightness") == 0 && cJSON_IsNumber(it)) {
            int b = it->valueint;
            if (b < 0) b = 0;
            if (b > 100) b = 100;
            ESP_LOGI(TAG, "Setting brightness = %d", b);
            dimmer_set_percent(b);
            changed = true;
            continue;
        }

        // Speed (0..5, clamp)
        if (strcmp(k, "speed") == 0 && cJSON_IsNumber(it)) {
            int s = it->valueint;
            if (s < 0) s = 0;
            if (s > 5) s = 5;
            fan_set_level(s);
            changed = true;
            continue;
        }
    }

    return changed;
}
// Handle new multi-command format
static bool handle_new_multi_cmd_json(cJSON *root) {
    if (!cJSON_IsObject(root)) return false;

    // Try "client_id"
    // cJSON *node = cJSON_GetObjectItem(root, "client_id");
    cJSON *node = NULL;

    // Try to find the first object value inside root
    cJSON *child = NULL;
    cJSON_ArrayForEach(child, root) {
        if (cJSON_IsObject(child)) {
            node = child;
            break;
        }
    }
if (!node) return false;
    if (!node) {
        // Try DEVICE_ID
        node = cJSON_GetObjectItem(root, DEVICE_ID);
    }
    if (!node || !cJSON_IsObject(node)) {
        return false;
    }

    bool changed_any = false;
    cJSON *floorK = NULL;
    cJSON_ArrayForEach(floorK, node) {
        if (!floorK->string) continue;
        if (!cJSON_IsObject(floorK)) continue;
        if (_apply_floor_object(floorK)) {
            changed_any = true;
        }
    }
    return changed_any;
}
// ---------- LEDC PWM ----------
static void pwm_init(void){
    // Dimmer timer
    ledc_timer_config_t t0 = {
        .speed_mode       = DIMMER_MODE,
        .duty_resolution  = LEDC_RES_BITS,
        .timer_num        = DIMMER_TIMER,
        .freq_hz          = DIMMER_FREQ_HZ,
        .clk_cfg          = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&t0));

    // Fan timer
    ledc_timer_config_t t1 = {
        .speed_mode       = FAN_MODE,
        .duty_resolution  = LEDC_RES_BITS,
        .timer_num        = FAN_TIMER,
        .freq_hz          = FAN_FREQ_HZ,
        .clk_cfg          = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&t1));

    // Dimmer channel
    ledc_channel_config_t ch_dimmer = {
        .gpio_num       = DIMMER_PIN,
        .speed_mode     = DIMMER_MODE,
        .channel        = DIMMER_CHANNEL,
        .intr_type      = LEDC_INTR_DISABLE,
        .timer_sel      = DIMMER_TIMER,
        .duty           = 0,
        .hpoint         = 0,
        .flags.output_invert = 0,
    };
    ESP_ERROR_CHECK(ledc_channel_config(&ch_dimmer));

    // Fan channel
    ledc_channel_config_t ch_fan = {
        .gpio_num       = FAN_PWM_PIN,
        .speed_mode     = FAN_MODE,
        .channel        = FAN_CHANNEL,
        .intr_type      = LEDC_INTR_DISABLE,
        .timer_sel      = FAN_TIMER,
        .duty           = 0,
        .hpoint         = 0,
        .flags.output_invert = 0,
    };
    ESP_ERROR_CHECK(ledc_channel_config(&ch_fan));
}
static void dimmer_set_percent(int percent){
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    g_dimmer_percent = percent;

    uint32_t duty = (percent * LEDC_MAX_DUTY) / 100;
    ESP_ERROR_CHECK(ledc_set_duty(DIMMER_MODE, DIMMER_CHANNEL, duty));
    ESP_ERROR_CHECK(ledc_update_duty(DIMMER_MODE, DIMMER_CHANNEL));
}
static void fan_set_level(int level){
    if (level < 0) level = 0;
    if (level > 5) level = 5;
    g_fan_level = level;

    static const int pct_map[6] = {0, 20, 40, 60, 80, 100};
    int percent = pct_map[level];
    uint32_t duty = (percent * LEDC_MAX_DUTY) / 100;

    ESP_ERROR_CHECK(ledc_set_duty(FAN_MODE, FAN_CHANNEL, duty));
    ESP_ERROR_CHECK(ledc_update_duty(FAN_MODE, FAN_CHANNEL));
}

// ---------- SNTP ----------
static void time_sync(void){
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_setservername(1, "time.google.com");
    esp_sntp_init();

    for (int i = 0; i < 12; i++){
        time_t now = 0; struct tm tm = {0};
        time(&now); gmtime_r(&now, &tm);
        if (tm.tm_year >= (2016 - 1900)){
            ESP_LOGI(TAG, "Time synced");
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    ESP_LOGW(TAG, "Time sync timeout; continuing");
}

// ---------- Wi-Fi NVS ----------
static void nvs_wifi_save(const char* ssid, const char* pass){
    nvs_handle_t nvh;
    ESP_ERROR_CHECK(nvs_open("wifi", NVS_READWRITE, &nvh));
    ESP_ERROR_CHECK(nvs_set_str(nvh, "ssid", ssid));
    ESP_ERROR_CHECK(nvs_set_str(nvh, "pass", pass));
    ESP_ERROR_CHECK(nvs_commit(nvh));
    nvs_close(nvh);
}
static bool nvs_wifi_load(char* ssid, size_t ssid_len, char* pass, size_t pass_len){
    nvs_handle_t nvh;
    if (nvs_open("wifi", NVS_READONLY, &nvh) != ESP_OK) return false;
    esp_err_t e1 = nvs_get_str(nvh, "ssid", ssid, &ssid_len);
    esp_err_t e2 = nvs_get_str(nvh, "pass", pass, &pass_len);
    nvs_close(nvh);
    return (e1 == ESP_OK && e2 == ESP_OK && ssid[0] != 0);
}
static void nvs_wifi_erase(void){
    nvs_handle_t nvh;
    if (nvs_open("wifi", NVS_READWRITE, &nvh) == ESP_OK){
        nvs_erase_key(nvh, "ssid");
        nvs_erase_key(nvh, "pass");
        nvs_commit(nvh);
        nvs_close(nvh);
    }
    esp_wifi_restore();
}

// ---------- Wi-Fi events ----------

static void wifi_evt(void* arg, esp_event_base_t base, int32_t id, void* data){
    if (base == WIFI_EVENT) {
        if (id == WIFI_EVENT_STA_START) {
            esp_wifi_connect();
        } else if (id == WIFI_EVENT_STA_DISCONNECTED) {
            wifi_connected = false;
            xEventGroupClearBits(s_wifi_eg, WIFI_CONNECTED_BIT);
            wifi_event_sta_disconnected_t *e = (wifi_event_sta_disconnected_t*)data;
            wifi_disconn_count++;
            last_wifi_disconn_tick = xTaskGetTickCount();
            ESP_LOGW(TAG, "Wi-Fi disconnected reason=%d (count=%d)", 
                     e ? e->reason : -1, wifi_disconn_count);
            // Immediate retry; supervisor will handle backoff/escalation
            vTaskDelay(pdMS_TO_TICKS(500));
            esp_wifi_connect();
        }
    } 
    else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        wifi_connected = true;
        wifi_disconn_count = 0;
        xEventGroupSetBits(s_wifi_eg, WIFI_CONNECTED_BIT);
    }
}

// ---------- DNS guard ----------
static bool wait_dns_for_host(const char* host, int retries, int delay_ms){
    for (int i = 0; i < retries; i++) {
        struct addrinfo *res = NULL;
        int ret = getaddrinfo(host, NULL, NULL, &res);
        if (ret == 0 && res) {
            freeaddrinfo(res);
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(delay_ms));
    }
    return false;
}

// ---------- SoftAP web (HTML + form) ----------
static const char PROV_HTML[] =
"<!doctype html><html><head><meta charset='utf-8'/>"
"<meta name='viewport' content='width=device-width,initial-scale=1'/>"
"<title>ESP32 Wi-Fi Setup</title>"
"<style>body{font-family:system-ui,Arial;margin:24px} .card{max-width:520px;padding:16px;border:1px solid #ddd;border-radius:12px}"
"input,button{padding:10px;margin:6px 0;width:100%} .btn{padding:12px;border-radius:10px;border:none;cursor:pointer;background:#111;color:#fff}</style>"
"</head><body><h2>ESP32 Wi-Fi Setup</h2><div class='card'>"
"<form method='POST' action='/provision'>"
"<label>SSID</label><input name='ssid' required/>"
"<label>Password</label><input name='password' type='password' required/>"
"<button type='submit' class='btn'>Save & Reboot</button>"
"</form></div></body></html>";

static esp_err_t root_get(httpd_req_t *req){
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, PROV_HTML, HTTPD_RESP_USE_STRLEN);
}
static esp_err_t prov_post(httpd_req_t *req){
    if (req->content_len <= 0 || req->content_len > 1024)
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad content length");

    char *body = malloc(req->content_len+1);
    if (!body) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");

    int recvd = 0;
    while (recvd < req->content_len) {
        int r = httpd_req_recv(req, body+recvd, req->content_len-recvd);
        if (r <= 0) { free(body); return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "recv fail"); }
        recvd += r;
    }
    body[recvd] = 0;

    // Extract URL-encoded form fields
    char ssid_enc[64]={0}, pass_enc[64]={0}, ssid[64]={0}, pass[64]={0};
    httpd_query_key_value(body, "ssid", ssid_enc, sizeof(ssid_enc));
    httpd_query_key_value(body, "password", pass_enc, sizeof(pass_enc));
    url_decode(ssid, ssid_enc);
    url_decode(pass, pass_enc);

    if (!ssid[0]) { free(body); return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing SSID"); }

    nvs_wifi_save(ssid, pass);

    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr(req, "<html><body><h3>✅ Saved! Rebooting…</h3></body></html>");
    free(body);
    vTaskDelay(pdMS_TO_TICKS(600));
    esp_restart();
    return ESP_OK;
}

static void start_http_server_for_wifi_provision(void){
    if (s_httpd) return;
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = 80;
    cfg.lru_purge_enable = true;
    ESP_ERROR_CHECK(httpd_start(&s_httpd, &cfg));
    httpd_uri_t u_root = { .uri="/", .method=HTTP_GET,  .handler=root_get };
    httpd_uri_t u_prov = { .uri="/provision", .method=HTTP_POST, .handler=prov_post };
    httpd_register_uri_handler(s_httpd, &u_root);
    httpd_register_uri_handler(s_httpd, &u_prov);
}

// ---------- SoftAP provisioning orchestrator ----------
static void start_softap_provisioning(void) {
    provisioning_mode = true;
    ESP_LOGW(TAG, "Starting SoftAP provisioning...");

    // Tear down any STA instance
    esp_wifi_stop();
    esp_wifi_deinit();

    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    // Unique SSID: MyDevice_Setup_XXXX (last 2 bytes of MAC)
    uint8_t mac[6]; esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char ssid_ap[32];
    snprintf(ssid_ap, sizeof(ssid_ap), "MyDevice_Setup_%02X%02X", mac[4], mac[5]);

    wifi_config_t ap = {0};
    strncpy((char*)ap.ap.ssid, ssid_ap, sizeof(ap.ap.ssid)-1);
    ap.ap.ssid_len = strlen((char*)ap.ap.ssid);
    ap.ap.channel = 1;
    ap.ap.max_connection = 4;
    ap.ap.authmode = WIFI_AUTH_OPEN; // open; easy for end users
    ap.ap.beacon_interval = 100;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));
    ESP_ERROR_CHECK(esp_wifi_start());

    // mDNS name also unique
    mdns_init();
    char host[32];
    snprintf(host, sizeof(host), "esp32-setup-%02x%02x", mac[4], mac[5]);
    mdns_hostname_set(host);
    mdns_instance_name_set("ESP32 Setup");

    start_http_server_for_wifi_provision();

    ESP_LOGW(TAG, "SoftAP started. SSID: %s (no password)", ssid_ap);
    ESP_LOGW(TAG, "Open http://192.168.4.1/ to provision Wi-Fi");
}

// ---------- Wi-Fi connect to saved creds (8 retries with backoff) ----------
static bool wifi_connect_saved(void){
    char ssid[64]={0}, pass[64]={0};
    if (!nvs_wifi_load(ssid, sizeof(ssid), pass, sizeof(pass))){
        ESP_LOGW(TAG, "No saved Wi-Fi; starting SoftAP provisioning");
        start_softap_provisioning();
        return false;
    }

    esp_netif_create_default_wifi_sta();

    wifi_init_config_t icfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&icfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_evt, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_evt, NULL, NULL));

    wifi_config_t st = {0};
    strncpy((char*)st.sta.ssid, ssid, sizeof(st.sta.ssid)-1);
    strncpy((char*)st.sta.password, pass, sizeof(st.sta.password)-1);
    st.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &st));
    ESP_ERROR_CHECK(esp_wifi_start());

    wifi_ps_type_t ps_type = WIFI_PS_MIN_MODEM;
    esp_wifi_set_ps(ps_type);

    int backoff_ms = 500;
    for (int attempt = 1; attempt <= 8; attempt++){
        ESP_LOGI(TAG, "Connecting to SSID='%s' (try %d/8)…", (char*)st.sta.ssid, attempt);
        esp_wifi_connect();

        EventBits_t bits = xEventGroupWaitBits(s_wifi_eg, WIFI_CONNECTED_BIT,
                                               pdFALSE, pdFALSE, pdMS_TO_TICKS(8000));
        if (bits & WIFI_CONNECTED_BIT){
            ESP_LOGI(TAG, "Wi-Fi connected");
            return true;
        }
        ESP_LOGW(TAG, "Wi-Fi not ready; retry in %d ms", backoff_ms);
        vTaskDelay(pdMS_TO_TICKS(backoff_ms));
        if (backoff_ms < 5000) backoff_ms += 500;
    }

    ESP_LOGE(TAG, "Wi-Fi failed after 8 retries -> SoftAP provisioning");
    start_softap_provisioning();
    return false;
}

// ---------- MQTT ----------
static void mqtt_publish_state(void){
    if (!s_mqtt) return;
    char rel[11]; relays_bits_str(rel);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "deviceId", DEVICE_ID);
    cJSON_AddStringToObject(root, "relays", rel);
    cJSON_AddNumberToObject(root, "dimmer", g_dimmer_percent);
    cJSON_AddNumberToObject(root, "fan_speed", g_fan_level);
    cJSON_AddStringToObject(root, "firmware", APP_VERSION);

    char *js = cJSON_PrintUnformatted(root);
    if (js){
        esp_mqtt_client_publish(s_mqtt, TOPIC_STATE, js, 0, 1, 0);
        cJSON_free(js);
    }
    cJSON_Delete(root);
}
static void mqtt_publish_heartbeat(void){
    if (!s_mqtt) return;
    char ts[25]; get_iso8601_utc(ts);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "deviceId", DEVICE_ID);
    cJSON_AddStringToObject(root, "timestamp", ts);
    cJSON_AddStringToObject(root, "firmware", APP_VERSION);
    cJSON_AddStringToObject(root, "status", mqtt_connected ? "OK" : "DISCONNECTED");
    cJSON_AddNumberToObject(root, "dimmer", g_dimmer_percent);
    cJSON_AddNumberToObject(root, "fan_speed", g_fan_level);

    char *js = cJSON_PrintUnformatted(root);
    if (js){
        esp_mqtt_client_publish(s_mqtt, TOPIC_HEARTBEAT, js, 0, 1, 1);
        cJSON_free(js);
    }
    cJSON_Delete(root);
}
static void mqtt_on(void *arg, esp_event_base_t base, int32_t eid, void *edata){
    esp_mqtt_event_handle_t ev = (esp_mqtt_event_handle_t)edata;
    switch (ev->event_id){
        case MQTT_EVENT_CONNECTED:
            mqtt_connected = true;
            ESP_LOGI(TAG, "MQTT connected");
            esp_mqtt_client_subscribe(s_mqtt, TOPIC_CMD, 1);
            mqtt_publish_state();
            break;

        case MQTT_EVENT_DISCONNECTED:
            mqtt_connected = false;
            ESP_LOGW(TAG, "MQTT disconnected");
            break;

        case MQTT_EVENT_DATA: {
                if (ev->topic_len && strncmp(ev->topic, TOPIC_STATE, ev->topic_len) == 0) {
                    // message came from our own published state topic
                    ESP_LOGW(TAG, "Ignoring self-published message on %.*s", ev->topic_len, ev->topic);
                    break;
                }
                ESP_LOGI(TAG, "MQTT_EVENT_DATA received, len=%d", ev->data_len);
                ESP_LOGI(TAG, "Payload: %.*s", ev->data_len, ev->data);
                bool changed = false;
                cJSON *cmd = cJSON_ParseWithLength(ev->data, ev->data_len);
                if (!cmd) {
                    ESP_LOGE(TAG, "JSON parse failed!");
                } else {
                    ESP_LOGI(TAG, "JSON parsed OK");
                }
                cJSON *test = NULL;
                ESP_LOGI(TAG, "Root object keys:");
                cJSON_ArrayForEach(test, cmd) {
                    ESP_LOGI(TAG, "Key = %s", test->string);
                }
                if (cmd) {
                    ESP_LOGI(TAG, "Payload: %.*s", ev->data_len, ev->data);
                    // 1) Try new multi-command format
                    bool handled_new = handle_new_multi_cmd_json(cmd);
                    ESP_LOGI(TAG, "handled_new=%d", handled_new);
                    if (handled_new) {
                        changed = true;
                    } else {
                        // 2) Old single-command format (unchanged)
                        cJSON *ja = cJSON_GetObjectItem(cmd, "action");
                        if (cJSON_IsString(ja) &&
                            strncmp(ja->valuestring, "reset_wifi", strlen("reset_wifi")) == 0)
                        {
                            ESP_LOGW(TAG, "Received Wi-Fi reset command");
                            nvs_wifi_erase();
                            esp_restart();
                        }

                        cJSON *jr = cJSON_GetObjectItem(cmd, "relay");
                        cJSON *js = cJSON_GetObjectItem(cmd, "state");
                        if (cJSON_IsNumber(jr) && cJSON_IsNumber(js)) {
                            relay_set_1based(jr->valueint, js->valueint ? 1 : 0);
                            changed = true;
                        }

                        cJSON *jdimmer = cJSON_GetObjectItem(cmd, "dimmer");
                        if (cJSON_IsNumber(jdimmer)) {
                            dimmer_set_percent(jdimmer->valueint);
                            changed = true;
                        }

                        cJSON *jfan = cJSON_GetObjectItem(cmd, "fan_speed");
                        if (cJSON_IsNumber(jfan)) {
                            int lvl = jfan->valueint;
                            if (lvl < 0) lvl = 0;
                            if (lvl > 5) lvl = 5;
                            fan_set_level(lvl);
                            changed = true;
                        }
                    }

                    cJSON_Delete(cmd);
                } else {
                    ESP_LOGW(TAG, "MQTT payload not JSON (len=%d)", ev->data_len);
                }

                if (changed) {
                    mqtt_publish_state();
                }
                vTaskDelay(pdMS_TO_TICKS(50));
                break;
            }


        case MQTT_EVENT_ERROR:
            ESP_LOGE(TAG, "MQTT error: type=%d", ev->error_handle ? ev->error_handle->error_type : -1);
            break;

        default:
            break;
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
        .session = { .keepalive = 60, .last_will = { .topic = TOPIC_HEARTBEAT, .msg = "{\"status\":\"offline\"}", .msg_len = 21, .qos = 1, .retain = 1 } },
        .network = { .reconnect_timeout_ms = 2000, .disable_auto_reconnect = false },
    };
    s_mqtt = esp_mqtt_client_init(&cfg);
    esp_mqtt_client_register_event(s_mqtt, ESP_EVENT_ANY_ID, mqtt_on, NULL);
    esp_mqtt_client_start(s_mqtt);
}

// ---------- OTA helpers ----------
static int cmp_semver(const char *a, const char *b){
    int A1=0,A2=0,A3=0, B1=0,B2=0,B3=0;
    sscanf(a,"%d.%d.%d",&A1,&A2,&A3);
    sscanf(b,"%d.%d.%d",&B1,&B2,&B3);
    if (A1!=B1) return (A1<B1)?-1:1;
    if (A2!=B2) return (A2<B2)?-1:1;
    if (A3!=B3) return (A3<B3)?-1:1;
    return 0;
}
static esp_err_t http_get_to_buf(const char *url, char *buf, size_t buflen){
    esp_http_client_config_t cfg = {
        .url = url,
        .cert_pem = (const char *)rootCA_pem_start,
        .timeout_ms = 15000,
        .keep_alive_enable = true,
        .disable_auto_redirect = false,
        .user_agent = "esp32-ota-client"
    };
    esp_http_client_handle_t cli = esp_http_client_init(&cfg);
    if (!cli) return ESP_FAIL;

    ESP_LOGI(TAG, "HTTP GET: %s", url);
    esp_task_wdt_reset();  // 🔹 feed before blocking

    esp_err_t err = esp_http_client_open(cli, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "http open failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(cli);
        return err;
    }

    esp_task_wdt_reset();  // 🔹 feed again before read
    esp_http_client_fetch_headers(cli);

    int http_code = esp_http_client_get_status_code(cli);
    ESP_LOGI(TAG, "HTTP status: %d", http_code);
    if (http_code != 200) {
        esp_http_client_close(cli);
        esp_http_client_cleanup(cli);
        return ESP_FAIL;
    }

    int total = 0, r;
    while ((r = esp_http_client_read(cli, buf + total, buflen - total - 1)) > 0) {
        total += r;
        esp_task_wdt_reset();       // 🔹 feed while reading chunks
        vTaskDelay(pdMS_TO_TICKS(50)); // let other tasks run
        if (total >= (int)buflen - 1) break;
    }
    buf[total] = 0;
    ESP_LOGI(TAG, "HTTP read %d bytes", total);

    esp_http_client_close(cli);
    esp_http_client_cleanup(cli);
    esp_task_wdt_reset();  // 🔹 final feed before return
    return (total > 0) ? ESP_OK : ESP_FAIL;
}

static esp_err_t perform_ota_from_url(const char *url){
    if (!url || !url[0]) return ESP_ERR_INVALID_ARG;

    esp_https_ota_config_t ota_cfg = {
        .http_config = &(esp_http_client_config_t){
            .url = url,
            .cert_pem = (const char *)rootCA_pem_start,
            .timeout_ms = 15000,
            .keep_alive_enable = true,
            .disable_auto_redirect = false,
            .user_agent = "esp32-ota-client",
        },
        .partial_http_download = false,
    };

    ESP_LOGI(TAG, "OTA: starting: %s", url);
    esp_err_t ret = esp_https_ota(&ota_cfg);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "OTA: success, rebooting");

        if (s_mqtt) {
            esp_mqtt_client_stop(s_mqtt);
            esp_mqtt_client_destroy(s_mqtt);
            s_mqtt = NULL;
        }
        esp_wifi_disconnect();
        vTaskDelay(pdMS_TO_TICKS(200));
        esp_restart();
    } else {
        ESP_LOGE(TAG, "OTA failed: %s", esp_err_to_name(ret));
    }
    return ret;
}
static void fetch_manifest_and_maybe_update(void){
    esp_task_wdt_reset();  // 🔹 add
    char json[1024] = {0};
    if (http_get_to_buf(OTA_MANIFEST_URL, json, sizeof(json)) != ESP_OK) {
        ESP_LOGW(TAG, "Manifest fetch failed");
        return;
    }
    ESP_LOGI(TAG, "Manifest JSON: %s", json);

    cJSON *root = cJSON_Parse(json);
    if (!root) { ESP_LOGW(TAG, "Manifest JSON parse failed"); return; }

    const cJSON *v = cJSON_GetObjectItem(root, "version");
    const cJSON *u = cJSON_GetObjectItem(root, "url");
    if (!cJSON_IsString(v) || !cJSON_IsString(u)) {
        ESP_LOGW(TAG, "Manifest missing version/url");
        cJSON_Delete(root);
        return;
    }

    const char *new_ver = v->valuestring;
    const char *url     = u->valuestring;
    const char *cur_ver = APP_VERSION;

    ESP_LOGI(TAG, "OTA: current=%s, new=%s", cur_ver, new_ver);
    if (cmp_semver(cur_ver, new_ver) < 0) {
        ESP_LOGI(TAG, "OTA: newer available, starting");
        perform_ota_from_url(url);
    } else {
        ESP_LOGI(TAG, "Already up-to-date");
    }
    cJSON_Delete(root);
    esp_task_wdt_reset();  // 🔹 feed after completion
}

static void ota_periodic_task(void *arg) {
    static bool first = true;
    if (first) {
        first = false;
        vTaskDelay(pdMS_TO_TICKS(10000));  // wait 10s after boot before first check
    }

    while (1) {
        esp_task_wdt_reset();
        ESP_LOGI(TAG, "OTA: Checking for updates...");
        fetch_manifest_and_maybe_update();
        esp_task_wdt_reset();

        // Wait for next OTA cycle (default every 30 minutes)
        for (int i = 0; i < 1800; i++) {
            esp_task_wdt_reset();
            vTaskDelay(pdMS_TO_TICKS(1000));  // 1s increments
        }
    }
}

// ---------- Post-OTA health & mark-valid ----------
static void post_ota_health_task(void *arg) {
    int checks = 0;
    const int max_checks = 30;  // check for ~30 seconds after boot

    while (1) {
        esp_task_wdt_reset();
        checks++;

        if (system_health_ok()) {
            ESP_LOGI(TAG, "OTA health OK, system stable");
            break;
        }

        if (checks > max_checks) {
            ESP_LOGE(TAG, "OTA health check failed, initiating rollback");
            esp_restart();
        }

        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    esp_task_wdt_reset();
    vTaskDelete(NULL);
}
static void rollback_mark_task(void *arg) {
    vTaskDelay(pdMS_TO_TICKS(10000));  // wait before marking
    esp_task_wdt_reset();

    esp_ota_img_states_t st;
    const esp_partition_t *part = esp_ota_get_running_partition();
    if (esp_ota_get_state_partition(part, &st) == ESP_OK &&
        st == ESP_OTA_IMG_PENDING_VERIFY) {
        ESP_LOGI(TAG, "Marking OTA image as valid...");
        esp_ota_mark_app_valid_cancel_rollback();
    } else {
        ESP_LOGI(TAG, "Image already marked valid.");
    }

    esp_task_wdt_reset();
    vTaskDelete(NULL);
}

// ---------- Heartbeat task ----------
static void heartbeat_task(void *arg) {
    const int heartbeat_interval_sec = 300;  // 5 minutes = 300 seconds
    const int wdt_feed_interval_sec  = 5;    // feed every 5 seconds

    int seconds_elapsed = 0;

    while (1) {
        esp_task_wdt_reset();

        // Publish heartbeat every 5 minutes
        if (seconds_elapsed >= heartbeat_interval_sec) {
            seconds_elapsed = 0;
            if (mqtt_is_connected()) {
                mqtt_publish_heartbeat();
                ESP_LOGI(TAG, "Heartbeat published (every 5 minutes).");
            } else {
                ESP_LOGW(TAG, "Heartbeat skipped (MQTT disconnected).");
            }
        }

        // Feed watchdog every 5 seconds
        for (int i = 0; i < wdt_feed_interval_sec; i++) {
            esp_task_wdt_reset();
            vTaskDelay(pdMS_TO_TICKS(1000));
            seconds_elapsed++;
        }
    }
}


// ---------- Connection Supervisor (Wi-Fi & MQTT) ----------
static void conn_supervisor_task(void *arg) {
    const TickType_t check_period = pdMS_TO_TICKS(2000);
    uint32_t wifi_backoff_ms = 2000;
    const uint32_t max_delay_ms = 30000;

    while (1) {
        esp_task_wdt_reset();  // 🔹 feed watchdog

        if (!wifi_is_connected()) {
            ESP_LOGW(TAG, "Supervisor: Wi-Fi down, retrying...");
            wifi_reconnect();
            vTaskDelay(pdMS_TO_TICKS(wifi_backoff_ms));
            wifi_backoff_ms = (wifi_backoff_ms * 2 > max_delay_ms)
                                ? max_delay_ms
                                : wifi_backoff_ms * 2;
            continue;
        }

        if (!mqtt_is_connected()) {
            ESP_LOGW(TAG, "Supervisor: MQTT down, retrying...");
            mqtt_reconnect();
        }

        // Reset backoff if stable
        wifi_backoff_ms = 2000;

        // Feed watchdog + sleep for normal monitoring
        for (int i = 0; i < 10; i++) {
            esp_task_wdt_reset();
            vTaskDelay(check_period);
        }
    }
}


// static void wdt_global_feed_task(void *arg) {
//     while (1) {
//         esp_task_wdt_reset();
//         vTaskDelay(pdMS_TO_TICKS(1500));  // feed every 1.5s
//     }
// }
static void init_task_wdt(void)
{
    const esp_task_wdt_config_t twdt_cfg = {
        .timeout_ms = 60000,               // 20s total
        .idle_core_mask = (1 << portNUM_PROCESSORS) - 1,
        .trigger_panic = true
    };

    esp_err_t err = esp_task_wdt_reconfigure(&twdt_cfg);
    if (err != ESP_OK) ESP_LOGW("WDT", "TWDT reconfig: %s", esp_err_to_name(err));
    esp_task_wdt_add(NULL);
}


// ---------- app_main ----------
void app_main(void) {
    ESP_LOGI(TAG, "FW %s", APP_VERSION);

    // --- Watchdog setup and global feed ---
    init_task_wdt();
    // --- Core system setup ---
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    s_wifi_eg = xEventGroupCreate();
    relays_init();
    pwm_init();
    dimmer_set_percent(0);
    fan_set_level(0);

    // --- Wi-Fi connect or SoftAP provisioning ---
    if (!wifi_connect_saved()) {
        ESP_LOGW(TAG, "Provisioning mode active; staying alive");
        while (1) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            esp_task_wdt_reset();
        }
    }

    // --- Time sync and DNS check before MQTT ---
    time_sync();

    if (!wait_dns_for_host(AWS_ENDPOINT_HOST, DNS_RETRY_COUNT, DNS_RETRY_DELAY_MS)) {
        ESP_LOGW(TAG, "DNS lookup failed repeatedly. Delaying MQTT start.");
        vTaskDelay(pdMS_TO_TICKS(5000));
    } else {
        mqtt_start();

        // Start connection supervisor
        TaskHandle_t h_conn_sup = NULL;
        xTaskCreatePinnedToCore(conn_supervisor_task, "conn_supervisor", 4096, NULL, 5, &h_conn_sup, tskNO_AFFINITY);
        if (h_conn_sup) esp_task_wdt_add(h_conn_sup);
    }

    // --- OTA periodic check task ---
    TaskHandle_t h_ota_periodic_task = NULL;
    xTaskCreatePinnedToCore(ota_periodic_task, "ota_check", 8192, NULL, 4, &h_ota_periodic_task, tskNO_AFFINITY);
    if (h_ota_periodic_task) esp_task_wdt_add(h_ota_periodic_task);

    // --- Rollback health check ---
    esp_ota_img_states_t st;
    const esp_partition_t *part = esp_ota_get_running_partition();
    if (esp_ota_get_state_partition(part, &st) == ESP_OK && st == ESP_OTA_IMG_PENDING_VERIFY) {
        ESP_LOGW(TAG, "Running image is PENDING_VERIFY; enabling health checks and mark-valid flow");
        TaskHandle_t post_ota_h = NULL;
        xTaskCreatePinnedToCore(post_ota_health_task, "ota_health", 4096, NULL, 5, &post_ota_h, tskNO_AFFINITY);
        if (post_ota_h) esp_task_wdt_add(post_ota_h);

        TaskHandle_t rollback_mark = NULL;
        xTaskCreatePinnedToCore(rollback_mark_task, "rb_mark", 3072, NULL, 5, &rollback_mark, tskNO_AFFINITY);
        if (rollback_mark) esp_task_wdt_add(rollback_mark);
    }

    // --- Heartbeat task ---
    TaskHandle_t heartbeat_t = NULL;
    xTaskCreatePinnedToCore(heartbeat_task, "heartbeat", 3072, NULL, 4, &heartbeat_t, tskNO_AFFINITY);
    if (heartbeat_t) esp_task_wdt_add(heartbeat_t);

    while (1) {
        esp_task_wdt_reset();
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}