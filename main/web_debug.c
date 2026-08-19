#include "web_debug.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/portmacro.h"
#include "usb/usb_host.h"

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAILED_BIT BIT1
#define WIFI_MAXIMUM_RETRIES 10
#define DEBUG_EVENT_CAPACITY 64
#define DEBUG_REPORT_BYTES 8

static const char *TAG = "web_debug";

extern const uint8_t debug_page_html_start[] asm("_binary_debug_page_html_start");
extern const uint8_t debug_page_html_end[] asm("_binary_debug_page_html_end");

typedef struct {
    uint32_t sequence;
    uint32_t milliseconds;
    uint8_t data[DEBUG_REPORT_BYTES];
} debug_event_t;

static portMUX_TYPE s_debug_lock = portMUX_INITIALIZER_UNLOCKED;
static debug_event_t s_events[DEBUG_EVENT_CAPACITY];
static uint32_t s_next_sequence = 1;
static size_t s_event_count;
static size_t s_event_write;
static uint8_t s_latest[DEBUG_REPORT_BYTES];
static uint32_t s_latest_ms;
static uint32_t s_report_count;
static size_t s_latest_length;
static uint16_t s_hid_vid;
static uint16_t s_hid_pid;
static uint8_t s_hid_address;
static uint8_t s_hid_interface;
static uint8_t s_hid_subclass;
static uint8_t s_hid_protocol;
static uint32_t s_enum_attempts;
static uint16_t s_enum_vid;
static uint16_t s_enum_pid;
static uint8_t s_enum_class;
static uint8_t s_enum_subclass;
static uint8_t s_enum_protocol;
static esp_err_t s_last_usb_error;
static char s_last_usb_stage[32] = "none";

static atomic_bool s_usb_host_ready;
static atomic_bool s_keyboard_online;
static atomic_bool s_hid_detected;
static atomic_bool s_ble_connected;
static atomic_bool s_ble_ready;
static EventGroupHandle_t s_wifi_events;
static esp_netif_t *s_wifi_netif;
static int s_wifi_retry_count;

static esp_err_t root_get_handler(httpd_req_t *request)
{
    const size_t length = debug_page_html_end - debug_page_html_start;
    httpd_resp_set_type(request, "text/html; charset=utf-8");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    return httpd_resp_send(request, (const char *)debug_page_html_start, length);
}

static uint32_t query_after_sequence(httpd_req_t *request)
{
    char query[48];
    char value[16];
    if (httpd_req_get_url_query_str(request, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, "after", value, sizeof(value)) != ESP_OK) {
        return 0;
    }
    char *end = NULL;
    unsigned long parsed = strtoul(value, &end, 10);
    return end != value && *end == '\0' ? (uint32_t)parsed : 0;
}

static esp_err_t status_get_handler(httpd_req_t *request)
{
    debug_event_t events[DEBUG_EVENT_CAPACITY];
    uint8_t latest[DEBUG_REPORT_BYTES];
    size_t copied = 0;
    uint32_t latest_ms;
    uint32_t report_count;
    size_t latest_length;
    uint16_t hid_vid;
    uint16_t hid_pid;
    uint8_t hid_address;
    uint8_t hid_interface;
    uint8_t hid_subclass;
    uint8_t hid_protocol;
    uint32_t enum_attempts;
    uint16_t enum_vid;
    uint16_t enum_pid;
    uint8_t enum_class;
    uint8_t enum_subclass;
    uint8_t enum_protocol;
    esp_err_t last_usb_error;
    char last_usb_stage[sizeof(s_last_usb_stage)];
    const uint32_t after = query_after_sequence(request);
    esp_netif_ip_info_t ip_info = {0};
    usb_host_lib_info_t usb_info = {0};
    if (s_wifi_netif != NULL) {
        esp_netif_get_ip_info(s_wifi_netif, &ip_info);
    }
    if (atomic_load(&s_usb_host_ready)) {
        usb_host_lib_info(&usb_info);
    }

    portENTER_CRITICAL(&s_debug_lock);
    memcpy(latest, s_latest, sizeof(latest));
    latest_ms = s_latest_ms;
    report_count = s_report_count;
    latest_length = s_latest_length;
    hid_vid = s_hid_vid;
    hid_pid = s_hid_pid;
    hid_address = s_hid_address;
    hid_interface = s_hid_interface;
    hid_subclass = s_hid_subclass;
    hid_protocol = s_hid_protocol;
    enum_attempts = s_enum_attempts;
    enum_vid = s_enum_vid;
    enum_pid = s_enum_pid;
    enum_class = s_enum_class;
    enum_subclass = s_enum_subclass;
    enum_protocol = s_enum_protocol;
    last_usb_error = s_last_usb_error;
    memcpy(last_usb_stage, s_last_usb_stage, sizeof(last_usb_stage));
    const size_t oldest = (s_event_write + DEBUG_EVENT_CAPACITY - s_event_count) %
                          DEBUG_EVENT_CAPACITY;
    for (size_t i = 0; i < s_event_count; ++i) {
        const debug_event_t event = s_events[(oldest + i) % DEBUG_EVENT_CAPACITY];
        if (event.sequence > after) {
            events[copied++] = event;
        }
    }
    portEXIT_CRITICAL(&s_debug_lock);

    httpd_resp_set_type(request, "application/json");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");

    char buffer[640];
    int length = snprintf(
        buffer, sizeof(buffer),
        "{\"usb_host_ready\":%s,\"hid_detected\":%s,\"keyboard_online\":%s,"
        "\"ble_connected\":%s,\"ble_ready\":%s,"
        "\"report_count\":%lu,\"report_length\":%u,\"latest_ms\":%lu,"
        "\"ip\":\"" IPSTR "\",\"vid\":%u,\"pid\":%u,"
        "\"usb_devices\":%d,\"usb_clients\":%d,"
        "\"enum_attempts\":%lu,\"enum_vid\":%u,\"enum_pid\":%u,"
        "\"enum_class\":%u,\"enum_subclass\":%u,\"enum_protocol\":%u,"
        "\"address\":%u,\"interface\":%u,\"subclass\":%u,\"protocol\":%u,"
        "\"error\":%ld,\"error_stage\":\"%s\","
        "\"latest\":[%u,%u,%u,%u,%u,%u,%u,%u],\"events\":[",
        atomic_load(&s_usb_host_ready) ? "true" : "false",
        atomic_load(&s_hid_detected) ? "true" : "false",
        atomic_load(&s_keyboard_online) ? "true" : "false",
        atomic_load(&s_ble_connected) ? "true" : "false",
        atomic_load(&s_ble_ready) ? "true" : "false",
        (unsigned long)report_count, (unsigned)latest_length,
        (unsigned long)latest_ms,
        IP2STR(&ip_info.ip),
        hid_vid, hid_pid, usb_info.num_devices, usb_info.num_clients,
        (unsigned long)enum_attempts, enum_vid, enum_pid,
        enum_class, enum_subclass, enum_protocol,
        hid_address, hid_interface, hid_subclass, hid_protocol,
        (long)last_usb_error, last_usb_stage,
        latest[0], latest[1], latest[2], latest[3],
        latest[4], latest[5], latest[6], latest[7]);
    if (length < 0 || (size_t)length >= sizeof(buffer) ||
        httpd_resp_send_chunk(request, buffer, length) != ESP_OK) {
        return ESP_FAIL;
    }

    for (size_t i = 0; i < copied; ++i) {
        const debug_event_t *event = &events[i];
        length = snprintf(
            buffer, sizeof(buffer),
            "%s{\"seq\":%lu,\"ms\":%lu,\"data\":[%u,%u,%u,%u,%u,%u,%u,%u]}",
            i == 0 ? "" : ",", (unsigned long)event->sequence,
            (unsigned long)event->milliseconds,
            event->data[0], event->data[1], event->data[2], event->data[3],
            event->data[4], event->data[5], event->data[6], event->data[7]);
        if (length < 0 || (size_t)length >= sizeof(buffer) ||
            httpd_resp_send_chunk(request, buffer, length) != ESP_OK) {
            return ESP_FAIL;
        }
    }
    if (httpd_resp_send_chunk(request, "]}", 2) != ESP_OK) {
        return ESP_FAIL;
    }
    return httpd_resp_send_chunk(request, NULL, 0);
}

static esp_err_t start_http_server(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 2;
    config.lru_purge_enable = true;
    config.stack_size = 6144;

    httpd_handle_t server = NULL;
    esp_err_t error = httpd_start(&server, &config);
    if (error != ESP_OK) {
        return error;
    }
    const httpd_uri_t root = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = root_get_handler,
    };
    const httpd_uri_t status = {
        .uri = "/api/status",
        .method = HTTP_GET,
        .handler = status_get_handler,
    };
    if ((error = httpd_register_uri_handler(server, &root)) != ESP_OK ||
        (error = httpd_register_uri_handler(server, &status)) != ESP_OK) {
        httpd_stop(server);
        return error;
    }
    return ESP_OK;
}

static void wifi_event_handler(void *argument, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    (void)argument;
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT &&
               event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_wifi_retry_count < WIFI_MAXIMUM_RETRIES) {
            s_wifi_retry_count++;
            esp_wifi_connect();
        } else {
            xEventGroupSetBits(s_wifi_events, WIFI_FAILED_BIT);
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *event = event_data;
        s_wifi_retry_count = 0;
        ESP_LOGI(TAG, "connected to '%s', open http://" IPSTR,
                 CARDPUTER_WIFI_SSID, IP2STR(&event->ip_info.ip));
        xEventGroupSetBits(s_wifi_events, WIFI_CONNECTED_BIT);
    }
}

esp_err_t web_debug_start(void)
{
    esp_err_t error = esp_netif_init();
    if (error != ESP_OK && error != ESP_ERR_INVALID_STATE) {
        return error;
    }
    error = esp_event_loop_create_default();
    if (error != ESP_OK && error != ESP_ERR_INVALID_STATE) {
        return error;
    }
    s_wifi_netif = esp_netif_create_default_wifi_sta();
    if (s_wifi_netif == NULL) {
        return ESP_ERR_NO_MEM;
    }
    esp_netif_set_hostname(s_wifi_netif, "hhkb-debug");
    s_wifi_events = xEventGroupCreate();
    if (s_wifi_events == NULL) {
        return ESP_ERR_NO_MEM;
    }

    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    if ((error = esp_wifi_init(&init)) != ESP_OK) {
        return error;
    }
    if ((error = esp_event_handler_register(
             WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL)) != ESP_OK ||
        (error = esp_event_handler_register(
             IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL)) != ESP_OK) {
        return error;
    }
    wifi_config_t station = {
        .sta = {
            .ssid = CARDPUTER_WIFI_SSID,
            .password = CARDPUTER_WIFI_PASSWORD,
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
            .pmf_cfg = {.required = false},
        },
    };
    if ((error = esp_wifi_set_mode(WIFI_MODE_STA)) != ESP_OK ||
        (error = esp_wifi_set_config(WIFI_IF_STA, &station)) != ESP_OK ||
        (error = esp_wifi_start()) != ESP_OK) {
        return error;
    }

    const EventBits_t bits = xEventGroupWaitBits(
        s_wifi_events, WIFI_CONNECTED_BIT | WIFI_FAILED_BIT,
        pdFALSE, pdFALSE, portMAX_DELAY);
    if ((bits & WIFI_CONNECTED_BIT) == 0) {
        ESP_LOGE(TAG, "failed to connect to Wi-Fi '%s'", CARDPUTER_WIFI_SSID);
        return ESP_FAIL;
    }
    return start_http_server();
}

void web_debug_set_usb_host_ready(bool ready)
{
    atomic_store(&s_usb_host_ready, ready);
}

void web_debug_set_keyboard_online(bool online)
{
    atomic_store(&s_keyboard_online, online);
}

void web_debug_set_ble_connected(bool connected)
{
    atomic_store(&s_ble_connected, connected);
}

void web_debug_set_ble_ready(bool ready)
{
    atomic_store(&s_ble_ready, ready);
}

void web_debug_note_hid_interface(uint16_t vid, uint16_t pid, uint8_t address,
                                  uint8_t interface_number, uint8_t subclass,
                                  uint8_t protocol)
{
    portENTER_CRITICAL(&s_debug_lock);
    s_hid_vid = vid;
    s_hid_pid = pid;
    s_hid_address = address;
    s_hid_interface = interface_number;
    s_hid_subclass = subclass;
    s_hid_protocol = protocol;
    portEXIT_CRITICAL(&s_debug_lock);
    atomic_store(&s_hid_detected, true);
}

void web_debug_note_usb_error(const char *stage, esp_err_t error)
{
    portENTER_CRITICAL(&s_debug_lock);
    s_last_usb_error = error;
    snprintf(s_last_usb_stage, sizeof(s_last_usb_stage), "%s",
             stage != NULL ? stage : "unknown");
    portEXIT_CRITICAL(&s_debug_lock);
}

void web_debug_note_enumeration(uint16_t vid, uint16_t pid, uint8_t device_class,
                                uint8_t subclass, uint8_t protocol)
{
    portENTER_CRITICAL(&s_debug_lock);
    s_enum_attempts++;
    s_enum_vid = vid;
    s_enum_pid = pid;
    s_enum_class = device_class;
    s_enum_subclass = subclass;
    s_enum_protocol = protocol;
    portEXIT_CRITICAL(&s_debug_lock);
}

void web_debug_record_report(const uint8_t *data, size_t length)
{
    if (data == NULL || length == 0) {
        return;
    }
    const uint32_t milliseconds = (uint32_t)(esp_timer_get_time() / 1000);

    portENTER_CRITICAL(&s_debug_lock);
    debug_event_t *event = &s_events[s_event_write];
    event->sequence = s_next_sequence++;
    event->milliseconds = milliseconds;
    memset(event->data, 0, DEBUG_REPORT_BYTES);
    memcpy(event->data, data,
           length < DEBUG_REPORT_BYTES ? length : DEBUG_REPORT_BYTES);
    memcpy(s_latest, event->data, DEBUG_REPORT_BYTES);
    s_latest_ms = milliseconds;
    s_latest_length = length;
    s_report_count++;
    s_event_write = (s_event_write + 1) % DEBUG_EVENT_CAPACITY;
    if (s_event_count < DEBUG_EVENT_CAPACITY) {
        s_event_count++;
    }
    portEXIT_CRITICAL(&s_debug_lock);
}
