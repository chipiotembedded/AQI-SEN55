#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "mqtt_client.h"

#include "sensirion_i2c_hal.h"
#include "sen5x_i2c.h"

#define TAG "SEN5X_MAIN"
#define SENSOR_POLL_DELAY_MS 10000  // 10 seconds

// WiFi network credentials
#define WIFI_SSID     "Ashutosh"
#define WIFI_PASSWORD "ashu1103"

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

static EventGroupHandle_t wifi_event_group;
static esp_mqtt_client_handle_t mqtt_client = NULL;
static int retry_count = 0;
#define MAX_RETRY 10

typedef struct {
    float conc_lo;
    float conc_hi;
    int aqi_lo;
    int aqi_hi;
} aqi_breakpoint_t;

// EPA breakpoints for PM2.5 (µg/m³)
const aqi_breakpoint_t pm25_breakpoints[] = {
    {   0.0f,   12.0f,   0,  50 },
    {  12.1f,   35.4f,  51, 100 },
    {  35.5f,   55.4f, 101, 150 },
    {  55.5f,  150.4f, 151, 200 },
    { 150.5f,  250.4f, 201, 300 },
    { 250.5f,  350.4f, 301, 400 },
    { 350.5f,  500.4f, 401, 500 }
};

// EPA breakpoints for PM10 (µg/m³)
const aqi_breakpoint_t pm10_breakpoints[] = {
    {    0.0f,   54.0f,   0,  50 },
    {   55.0f,  154.0f,  51, 100 },
    {  155.0f,  254.0f, 101, 150 },
    {  255.0f,  354.0f, 151, 200 },
    {  355.0f,  424.0f, 201, 300 },
    {  425.0f,  504.0f, 301, 400 },
    {  505.0f,  604.0f, 401, 500 }
};

// EPA breakpoints for NO2 (proxy for NOx) in ppb
const aqi_breakpoint_t nox_breakpoints[] = {
    {    0.0f,   53.0f,   0,  50 },
    {   54.0f,  100.0f,  51, 100 },
    {  101.0f,  360.0f, 101, 150 },
    {  361.0f,  649.0f, 151, 200 },
    {  650.0f, 1249.0f, 201, 300 },
    { 1250.0f, 1649.0f, 301, 400 },
    { 1650.0f, 2049.0f, 401, 500 }
};

// Calculate AQI for a pollutant
static int calculate_aqi(float conc, const aqi_breakpoint_t *bps, int len) {
    for (int i = 0; i < len; i++) {
        if (conc >= bps[i].conc_lo && conc <= bps[i].conc_hi) {
            float c_low = bps[i].conc_lo;
            float c_high = bps[i].conc_hi;
            int i_low   = bps[i].aqi_lo;
            int i_high  = bps[i].aqi_hi;
            float aqi = ((i_high - i_low) / (c_high - c_low)) * (conc - c_low) + i_low;
            return (int)(aqi + 0.5f);
        }
    }
    return -1;
}

// MQTT event handler
static void mqtt_event_handler(void *args, esp_event_base_t base, int32_t id, void *data) {
    esp_mqtt_event_handle_t event = data;
    switch (event->event_id) {
        case MQTT_EVENT_CONNECTED:    ESP_LOGI(TAG, "MQTT connected"); break;
        case MQTT_EVENT_DISCONNECTED: ESP_LOGI(TAG, "MQTT disconnected"); break;
        default: break;
    }
}

// Wi-Fi event handler
static void wifi_event_handler(void* arg, esp_event_base_t eb, int32_t id, void* data) {
    if (eb == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (eb == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        if (retry_count < MAX_RETRY) {
            ESP_LOGW(TAG, "Wi-Fi disconnected, reconnecting to %s... (attempt %d/%d)", WIFI_SSID, retry_count + 1, MAX_RETRY);
            esp_wifi_connect();
            retry_count++;
        } else {
            ESP_LOGE(TAG, "Failed to connect to %s after %d attempts", WIFI_SSID, MAX_RETRY);
            xEventGroupSetBits(wifi_event_group, WIFI_FAIL_BIT);
        }
    } else if (eb == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* e = data;
        ESP_LOGI(TAG, "Connected to %s - Got IP: " IPSTR, WIFI_SSID, IP2STR(&e->ip_info.ip));
        retry_count = 0;
        xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

// Initialize Wi-Fi
void wifi_init(void) {
    wifi_event_group = xEventGroupCreate();
    
    // Initialize NVS - handle errors gracefully to preserve data across flashes
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        // NVS partition was truncated and needs to be erased
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL));
    
    // Configure WiFi for single network
    wifi_config_t wcfg = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASSWORD,
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        }
    };
    
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wcfg));
    ESP_ERROR_CHECK(esp_wifi_start());
    
    ESP_LOGI(TAG, "Wi-Fi initialized, connecting to %s...", WIFI_SSID);
    
    // Wait for connection
    EventBits_t bits = xEventGroupWaitBits(
        wifi_event_group,
        WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
        pdFALSE,
        pdFALSE,
        portMAX_DELAY
    );
    
    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "Successfully connected to %s", WIFI_SSID);
    } else {
        ESP_LOGE(TAG, "Failed to connect to %s", WIFI_SSID);
    }
}

// Initialize MQTT
void mqtt_init(void) {
    const esp_mqtt_client_config_t cfg = { .broker.address.uri = "mqtt://broker.hivemq.com:1883" };
    mqtt_client = esp_mqtt_client_init(&cfg);
    esp_mqtt_client_register_event(mqtt_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    esp_mqtt_client_start(mqtt_client);
}

void app_main(void) {
    wifi_init();
    mqtt_init();
    sensirion_i2c_hal_init();

    ESP_LOGI(TAG, "Starting SEN5x measurement...");
    if (sen5x_start_measurement() != 0) {
        ESP_LOGE(TAG, "Failed to start measurement");
        return;
    }
    vTaskDelay(pdMS_TO_TICKS(1000));

    while (1) {
        bool ready = false;
        if (sen5x_read_data_ready(&ready) != 0) {
            ESP_LOGE(TAG, "Data-ready check failed");
        } else if (ready) {
            uint16_t pm1p0, pm2p5, pm4p0, pm10p0;
            int16_t hum, temp_raw, voc_idx, nox_idx;
            if (sen5x_read_measured_values(&pm1p0, &pm2p5, &pm4p0, &pm10p0, &hum, &temp_raw, &voc_idx, &nox_idx) == 0) {
                float pm1   = pm1p0 / 10.0f;
                float pm2_5 = pm2p5 / 10.0f;
                float pm4   = pm4p0 / 10.0f;
                float pm10  = pm10p0 / 10.0f;
                float temp  = temp_raw / 200.0f;
                float rh    = hum / 100.0f;
                float voc   = voc_idx / 10.0f;
                float nox   = nox_idx / 10.0f;

                int aqi25 = calculate_aqi(pm2_5, pm25_breakpoints, sizeof(pm25_breakpoints)/sizeof(pm25_breakpoints[0]));
                int aqi10 = calculate_aqi(pm10, pm10_breakpoints, sizeof(pm10_breakpoints)/sizeof(pm10_breakpoints[0]));
                int aqi_nox = calculate_aqi(nox, nox_breakpoints, sizeof(nox_breakpoints)/sizeof(nox_breakpoints[0]));
                int aqi = aqi25;
                if (aqi10 > aqi) aqi = aqi10;
                if (aqi_nox > aqi) aqi = aqi_nox;

                ESP_LOGI(TAG,
                         "PM1:%.1f PM2.5:%.1f PM4:%.1f PM10:%.1f Temp:%.2f°C RH:%.2f%% VOC:%.1f NOx:%.1f AQI25:%d AQI10:%d AQINOx:%d -> AQI:%d",
                         pm1, pm2_5, pm4, pm10, temp, rh, voc, nox, aqi25, aqi10, aqi_nox, aqi);

                if (mqtt_client) {
                    char payload[256];
                    snprintf(payload, sizeof(payload),
                             "{\"pm1\":%.1f,\"pm2_5\":%.1f,\"pm4\":%.1f,\"pm10\":%.1f,"
                             "\"temp\":%.2f,\"rh\":%.2f,\"voc\":%.1f,\"nox\":%.1f,\"aqi\":%d}",
                             pm1, pm2_5, pm4, pm10, temp, rh, voc, nox, aqi);
                    esp_mqtt_client_publish(mqtt_client, "your topic", payload, 0, 1, 0);
                }
            } else {
                ESP_LOGE(TAG, "Failed to read sensor values");
            }
        } else {
            ESP_LOGW(TAG, "Data not ready");
        }
        vTaskDelay(pdMS_TO_TICKS(SENSOR_POLL_DELAY_MS));
    }
}
