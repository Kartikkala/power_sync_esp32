#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "nvs_flash.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_http_server.h"

// --- THE NEW V6.X PROVISIONING HEADERS ---
#include "network_provisioning/manager.h"
#include "network_provisioning/scheme_softap.h"

static const char *TAG = "PowerSync_Node";

#define API_ENDPOINT   "http://192.168.1.5:8999/esp"

// --- Hardware Pins ---
#define RELAY_PIN      5
#define TXD_PIN        17 
#define RXD_PIN        16 
#define UART_NUM       UART_NUM_2
#define MAX_CURRENT_THRESHOLD 5.0

// --- ESP32 API Endpoints ---

// Handler for HTTP GET /on
static esp_err_t relay_on_handler(httpd_req_t *req) {
    ESP_LOGI(TAG, "🌐 API Command Received: Turn Relay ON");
    gpio_set_level(RELAY_PIN, 0); // 0 = ON (Power flows)
    httpd_resp_send(req, "{\"status\":\"success\", \"relay\":\"ON\"}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

// Handler for HTTP GET /off
static esp_err_t relay_off_handler(httpd_req_t *req) {
    ESP_LOGI(TAG, "🌐 API Command Received: Turn Relay OFF");
    gpio_set_level(RELAY_PIN, 1); // 1 = OFF (Power cut)
    httpd_resp_send(req, "{\"status\":\"success\", \"relay\":\"OFF\"}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

// Start the Web Server
static httpd_handle_t start_webserver(void) {
    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    // Default port is 80

    ESP_LOGI(TAG, "Starting HTTP Server on port: '%d'", config.server_port);
    if (httpd_start(&server, &config) == ESP_OK) {
        // Register URI handlers
        httpd_uri_t uri_on = { .uri = "/on", .method = HTTP_GET, .handler = relay_on_handler, .user_ctx = NULL };
        httpd_uri_t uri_off = { .uri = "/off", .method = HTTP_GET, .handler = relay_off_handler, .user_ctx = NULL };
        
        httpd_register_uri_handler(server, &uri_on);
        httpd_register_uri_handler(server, &uri_off);
        return server;
    }
    ESP_LOGE(TAG, "Error starting server!");
    return NULL;
}

// --- Event Handlers for Wi-Fi & Provisioning ---
static void event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    if (event_base == NETWORK_PROV_EVENT) {
        switch (event_id) {
            case NETWORK_PROV_START:
                ESP_LOGI(TAG, "Provisioning started. Connect your phone to 'PowerSync-Setup'");
                break;
            // FIXED: Using the _WIFI_ specific event flags for v6.x
            case NETWORK_PROV_WIFI_CRED_RECV:
                ESP_LOGI(TAG, "Received Wi-Fi credentials from phone!");
                break;
            case NETWORK_PROV_WIFI_CRED_SUCCESS:
                ESP_LOGI(TAG, "Provisioning successful!");
                break;
            case NETWORK_PROV_END:
                ESP_LOGI(TAG, "Provisioning session ended. Manager shutting down.");
                network_prov_mgr_deinit();
                break;
        }
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGI(TAG, "Disconnected. Retrying Wi-Fi connection...");
        esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "Brilliant! Connected with IP: " IPSTR, IP2STR(&event->ip_info.ip));
        start_webserver();
    }
}

// --- Provisioning Initialization ---
void start_wifi_provisioning(void) {
    esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);

    // Register event handlers
    esp_event_handler_register(NETWORK_PROV_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL);
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL);

    // Initialize the New Provisioning Manager
    network_prov_mgr_config_t config = {
        .scheme = network_prov_scheme_softap,
        .scheme_event_handler = NETWORK_PROV_EVENT_HANDLER_NONE
    };
    network_prov_mgr_init(config);

    bool provisioned = false;
    // Check if Wi-Fi credentials already exist in NVS
    network_prov_mgr_is_wifi_provisioned(&provisioned);

    if (!provisioned) {
        ESP_LOGI(TAG, "Starting Wi-Fi Provisioning via SoftAP...");
        
	    // This will now compile perfectly and accept a standard string!
        network_prov_security_t security = NETWORK_PROV_SECURITY_1;       
        // FIXED: This is the PIN you will type into the Mobile App to authorize the payload
        const char *pop = "powersync"; 
        
        // FIXED: Launch SoftAP with the correct v6.x function name
        network_prov_mgr_start_provisioning(security, (const void *)pop, "PowerSync-Setup", NULL);
    } else {
        ESP_LOGI(TAG, "Already provisioned, starting Wi-Fi STA normally.");
        network_prov_mgr_deinit();
        esp_wifi_set_mode(WIFI_MODE_STA);
        esp_wifi_start();
    }
}

// --- HTTP POST Function ---
void post_telemetry(float voltage, float current, float power, float energy) {
    char post_data[200];
    snprintf(post_data, sizeof(post_data), 
             "{\"nodeId\":\"Room_101\",\"voltage\":%.2f,\"current\":%.2f,\"power\":%.2f,\"energy\":%.3f}", 
             voltage, current, power, energy);

    esp_http_client_config_t config = {
        .url = API_ENDPOINT,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 3000,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, post_data, strlen(post_data));

    esp_err_t err = esp_http_client_perform(client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP POST failed: %s", esp_err_to_name(err));
    }
    esp_http_client_cleanup(client);
}

// --- Main Telemetry Task ---
void telemetry_task(void *pvParameters) {
    uart_config_t uart_config = {
        .baud_rate = 9600,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE
    };
    uart_param_config(UART_NUM, &uart_config);
    uart_set_pin(UART_NUM, TXD_PIN, RXD_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    uart_driver_install(UART_NUM, 256, 0, 0, NULL, 0);

    uint8_t pzem_req[] = {0x01, 0x04, 0x00, 0x00, 0x00, 0x0A, 0x70, 0x0D};
    uint8_t data[128];

    while (1) {
        uart_write_bytes(UART_NUM, (const char *)pzem_req, sizeof(pzem_req));
        vTaskDelay(100 / portTICK_PERIOD_MS); 

        int length = uart_read_bytes(UART_NUM, data, sizeof(data), 100 / portTICK_PERIOD_MS);
        
        if (length >= 25) { 
            float voltage = ((data[3] << 8) | data[4]) / 10.0;
            float current = (((data[5] << 8) | data[6]) | ((data[7] << 8) | data[8]) << 16) / 1000.0;
            float power = (((data[9] << 8) | data[10]) | ((data[11] << 8) | data[12]) << 16) / 10.0;
            float energy = (((data[13] << 8) | data[14]) | ((data[15] << 8) | data[16]) << 16) / 1000.0;

            if (current > MAX_CURRENT_THRESHOLD) {
                gpio_set_level(RELAY_PIN, 0); // Autocut
            }

            post_telemetry(voltage, current, power, energy);
        }
        vTaskDelay(5000 / portTICK_PERIOD_MS);
    }
}

// --- The ESP-IDF Entry Point ---
void app_main(void) {
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
      ESP_ERROR_CHECK(nvs_flash_erase());
      ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    esp_netif_init();
    esp_event_loop_create_default();

    gpio_reset_pin(RELAY_PIN);
    gpio_set_direction(RELAY_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(RELAY_PIN, 0); 

    // Start the v6.x Provisioning Logic
    start_wifi_provisioning();

    xTaskCreate(telemetry_task, "telemetry_task", 4096, NULL, 5, NULL);
}
