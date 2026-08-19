#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>
#include <string.h>

#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_gap_ble_api.h"
#include "esp_gatts_api.h"
#include "esp_hid_common.h"
#include "esp_hidd.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "usb/hid_host.h"
#include "usb/hid_usage_keyboard.h"
#include "usb/usb_host.h"
#include "web_debug.h"

#define BLE_DEVICE_NAME "HHKB_BLE"
#define HID_REPORT_ID_KEYBOARD 1
#define KEYBOARD_REPORT_BYTES 8
#define REPORT_QUEUE_DEPTH 32
#define USB_EVENT_QUEUE_DEPTH 16

static const char *TAG = "hhkb_bridge";

typedef struct {
    hid_host_device_handle_t handle;
    hid_host_driver_event_t event;
} usb_device_event_t;

typedef struct {
    uint8_t bytes[KEYBOARD_REPORT_BYTES];
} keyboard_report_t;

static QueueHandle_t s_usb_event_queue;
static QueueHandle_t s_report_queue;
static QueueHandle_t s_led_queue;
static SemaphoreHandle_t s_state_lock;

static esp_hidd_dev_t *s_ble_hid;
static hid_host_device_handle_t s_usb_keyboard;
static atomic_bool s_ble_connected;
static atomic_bool s_ble_ready;
static atomic_bool s_ble_hid_started;
static atomic_bool s_ble_adv_data_ready;
static keyboard_report_t s_latest_report;

/* Standard boot-keyboard report, widened to include international usages. */
static const uint8_t s_keyboard_report_map[] = {
    0x05, 0x01,              /* Usage Page (Generic Desktop) */
    0x09, 0x06,              /* Usage (Keyboard) */
    0xA1, 0x01,              /* Collection (Application) */
    0x85, HID_REPORT_ID_KEYBOARD,
    0x05, 0x07,              /* Usage Page (Keyboard/Keypad) */
    0x19, 0xE0,
    0x29, 0xE7,
    0x15, 0x00,
    0x25, 0x01,
    0x75, 0x01,
    0x95, 0x08,
    0x81, 0x02,              /* Modifier byte */
    0x95, 0x01,
    0x75, 0x08,
    0x81, 0x03,              /* Reserved byte */
    0x95, 0x05,
    0x75, 0x01,
    0x05, 0x08,              /* Usage Page (LEDs) */
    0x19, 0x01,
    0x29, 0x05,
    0x91, 0x02,
    0x95, 0x01,
    0x75, 0x03,
    0x91, 0x03,
    0x95, 0x06,
    0x75, 0x08,
    0x15, 0x00,
    0x26, 0xFF, 0x00,
    0x05, 0x07,
    0x19, 0x00,
    0x2A, 0xFF, 0x00,
    0x81, 0x00,              /* Six-key array */
    0xC0,
};

static esp_hid_raw_report_map_t s_report_maps[] = {
    {
        .data = s_keyboard_report_map,
        .len = sizeof(s_keyboard_report_map),
    },
};

static esp_hid_device_config_t s_ble_hid_config = {
    .vendor_id = 0x303A,      /* Espressif USB vendor ID */
    .product_id = 0x4002,
    .version = 0x0100,
    .device_name = BLE_DEVICE_NAME,
    .manufacturer_name = "HHKB BLE Bridge",
    .serial_number = "HHKB-BLE-01",
    .report_maps = s_report_maps,
    .report_maps_len = 1,
};

static esp_ble_adv_data_t s_adv_data = {
    .set_scan_rsp = false,
    .include_name = true,
    .include_txpower = true,
    .min_interval = 0x0006,
    .max_interval = 0x0010,
    .appearance = ESP_HID_APPEARANCE_KEYBOARD,
    .manufacturer_len = 0,
    .p_manufacturer_data = NULL,
    .service_data_len = 0,
    .p_service_data = NULL,
    .service_uuid_len = 16,
    .p_service_uuid = (uint8_t[]){
        0xFB, 0x34, 0x9B, 0x5F, 0x80, 0x00, 0x00, 0x80,
        0x00, 0x10, 0x00, 0x00, 0x12, 0x18, 0x00, 0x00,
    },
    .flag = ESP_BLE_ADV_FLAG_GEN_DISC | ESP_BLE_ADV_FLAG_BREDR_NOT_SPT,
};

static esp_ble_adv_params_t s_adv_params = {
    .adv_int_min = 0x20,
    .adv_int_max = 0x30,
    .adv_type = ADV_TYPE_IND,
    .own_addr_type = BLE_ADDR_TYPE_PUBLIC,
    .channel_map = ADV_CHNL_ALL,
    .adv_filter_policy = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY,
};

static void maybe_start_advertising(void)
{
    if (!atomic_load(&s_ble_hid_started) ||
        !atomic_load(&s_ble_adv_data_ready) ||
        atomic_load(&s_ble_connected)) {
        return;
    }
    esp_err_t err = esp_ble_gap_start_advertising(&s_adv_params);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "start advertising failed: %s", esp_err_to_name(err));
    }
}

static void queue_latest_report(void)
{
    keyboard_report_t report;
    xSemaphoreTake(s_state_lock, portMAX_DELAY);
    report = s_latest_report;
    xSemaphoreGive(s_state_lock);
    xQueueSend(s_report_queue, &report, 0);
}

static void ble_gap_callback(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param)
{
    switch (event) {
    case ESP_GAP_BLE_ADV_DATA_SET_COMPLETE_EVT:
        atomic_store(&s_ble_adv_data_ready, true);
        maybe_start_advertising();
        break;
    case ESP_GAP_BLE_SEC_REQ_EVT:
        esp_ble_gap_security_rsp(param->ble_security.ble_req.bd_addr, true);
        break;
    case ESP_GAP_BLE_AUTH_CMPL_EVT:
        atomic_store(&s_ble_ready, param->ble_security.auth_cmpl.success);
        web_debug_set_ble_ready(param->ble_security.auth_cmpl.success);
        if (atomic_load(&s_ble_ready)) {
            ESP_LOGI(TAG, "BLE bonded and ready");
            queue_latest_report();
        } else {
            ESP_LOGE(TAG, "BLE authentication failed: 0x%x",
                     param->ble_security.auth_cmpl.fail_reason);
        }
        break;
    default:
        break;
    }
}

static void ble_hid_callback(void *handler_args, esp_event_base_t base,
                             int32_t id, void *event_data)
{
    (void)handler_args;
    (void)base;
    esp_hidd_event_data_t *param = event_data;

    switch ((esp_hidd_event_t)id) {
    case ESP_HIDD_START_EVENT:
        atomic_store(&s_ble_hid_started, true);
        ESP_LOGI(TAG, "BLE HID started as '%s'", BLE_DEVICE_NAME);
        maybe_start_advertising();
        break;
    case ESP_HIDD_CONNECT_EVENT:
        atomic_store(&s_ble_connected, true);
        atomic_store(&s_ble_ready, false);
        web_debug_set_ble_connected(true);
        web_debug_set_ble_ready(false);
        ESP_LOGI(TAG, "BLE host connected; waiting for encryption");
        break;
    case ESP_HIDD_OUTPUT_EVENT:
        if (param->output.length > 0) {
            uint8_t leds = param->output.data[0];
            xQueueOverwrite(s_led_queue, &leds);
        }
        break;
    case ESP_HIDD_DISCONNECT_EVENT:
        atomic_store(&s_ble_connected, false);
        atomic_store(&s_ble_ready, false);
        web_debug_set_ble_connected(false);
        web_debug_set_ble_ready(false);
        ESP_LOGI(TAG, "BLE host disconnected");
        maybe_start_advertising();
        break;
    default:
        break;
    }
}

static void ble_init(void)
{
    esp_bt_controller_config_t controller_config = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT));
    ESP_ERROR_CHECK(esp_bt_controller_init(&controller_config));
    ESP_ERROR_CHECK(esp_bt_controller_enable(ESP_BT_MODE_BLE));

    esp_bluedroid_config_t bluedroid_config = BT_BLUEDROID_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_bluedroid_init_with_cfg(&bluedroid_config));
    ESP_ERROR_CHECK(esp_bluedroid_enable());
    ESP_ERROR_CHECK(esp_ble_gap_register_callback(ble_gap_callback));

    esp_ble_auth_req_t auth = ESP_LE_AUTH_REQ_SC_BOND;
    esp_ble_io_cap_t io_capability = ESP_IO_CAP_NONE;
    uint8_t key_size = 16;
    uint8_t keys = ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK;
    ESP_ERROR_CHECK(esp_ble_gap_set_security_param(ESP_BLE_SM_AUTHEN_REQ_MODE,
                                                   &auth, sizeof(auth)));
    ESP_ERROR_CHECK(esp_ble_gap_set_security_param(ESP_BLE_SM_IOCAP_MODE,
                                                   &io_capability, sizeof(io_capability)));
    ESP_ERROR_CHECK(esp_ble_gap_set_security_param(ESP_BLE_SM_MAX_KEY_SIZE,
                                                   &key_size, sizeof(key_size)));
    ESP_ERROR_CHECK(esp_ble_gap_set_security_param(ESP_BLE_SM_SET_INIT_KEY,
                                                   &keys, sizeof(keys)));
    ESP_ERROR_CHECK(esp_ble_gap_set_security_param(ESP_BLE_SM_SET_RSP_KEY,
                                                   &keys, sizeof(keys)));

    ESP_ERROR_CHECK(esp_ble_gap_set_device_name(BLE_DEVICE_NAME));
    ESP_ERROR_CHECK(esp_ble_gap_config_adv_data(&s_adv_data));
    ESP_ERROR_CHECK(esp_ble_gatts_register_callback(esp_hidd_gatts_event_handler));
    ESP_ERROR_CHECK(esp_hidd_dev_init(&s_ble_hid_config, ESP_HID_TRANSPORT_BLE,
                                      ble_hid_callback, &s_ble_hid));
    ESP_ERROR_CHECK(esp_hidd_dev_battery_set(s_ble_hid, 100));
}

static void enqueue_keyboard_report(const uint8_t *data, size_t length)
{
    if (length < KEYBOARD_REPORT_BYTES) {
        ESP_LOGW(TAG, "short keyboard report: %u", (unsigned)length);
        return;
    }

    keyboard_report_t report;
    memcpy(report.bytes, data, sizeof(report.bytes));

    xSemaphoreTake(s_state_lock, portMAX_DELAY);
    s_latest_report = report;
    xSemaphoreGive(s_state_lock);

    if (xQueueSend(s_report_queue, &report, 0) != pdTRUE) {
        xQueueReset(s_report_queue);
        xQueueSend(s_report_queue, &report, 0);
        ESP_LOGW(TAG, "report queue overflow; resynchronized");
    }
}

static void usb_interface_callback(hid_host_device_handle_t handle,
                                   hid_host_interface_event_t event, void *arg)
{
    (void)arg;
    hid_host_dev_params_t params;
    if (hid_host_device_get_params(handle, &params) != ESP_OK) {
        return;
    }

    if (event == HID_HOST_INTERFACE_EVENT_INPUT_REPORT) {
        uint8_t data[64];
        size_t length = 0;
        if (hid_host_device_get_raw_input_report_data(handle, data, sizeof(data),
                                                      &length) == ESP_OK) {
            web_debug_record_report(data, length);
            if (params.proto == HID_PROTOCOL_KEYBOARD) {
                enqueue_keyboard_report(data, length);
            }
        }
    } else if (event == HID_HOST_INTERFACE_EVENT_DISCONNECTED) {
        ESP_LOGI(TAG, "USB keyboard disconnected");
        bool was_keyboard = false;
        xSemaphoreTake(s_state_lock, portMAX_DELAY);
        if (s_usb_keyboard == handle) {
            s_usb_keyboard = NULL;
            was_keyboard = true;
        }
        xSemaphoreGive(s_state_lock);
        if (was_keyboard) {
            web_debug_set_keyboard_online(false);
            const keyboard_report_t released = {0};
            enqueue_keyboard_report(released.bytes, sizeof(released.bytes));
        }
        ESP_ERROR_CHECK_WITHOUT_ABORT(hid_host_device_close(handle));
    } else if (event == HID_HOST_INTERFACE_EVENT_TRANSFER_ERROR) {
        web_debug_note_usb_error("input_transfer", ESP_FAIL);
        ESP_LOGW(TAG, "USB HID transfer error");
    }
}

static void usb_open_device(hid_host_device_handle_t handle)
{
    hid_host_dev_params_t params;
    esp_err_t error = hid_host_device_get_params(handle, &params);
    if (error != ESP_OK) {
        web_debug_note_usb_error("get_params", error);
        ESP_LOGE(TAG, "get HID parameters failed: %s", esp_err_to_name(error));
        return;
    }
    if (params.proto != HID_PROTOCOL_KEYBOARD) {
        ESP_LOGI(TAG, "ignoring non-keyboard HID interface: addr=%u iface=%u protocol=%u",
                 params.addr, params.iface_num, params.proto);
        return;
    }
    web_debug_note_hid_interface(0, 0, params.addr, params.iface_num,
                                 params.sub_class, params.proto);

    const hid_host_device_config_t config = {
        .callback = usb_interface_callback,
        .callback_arg = NULL,
    };
    error = hid_host_device_open(handle, &config);
    if (error != ESP_OK) {
        web_debug_note_usb_error("device_open", error);
        ESP_LOGE(TAG, "open HID interface failed: %s", esp_err_to_name(error));
        return;
    }

    hid_host_dev_info_t info = {0};
    error = hid_host_get_device_info(handle, &info);
    if (error == ESP_OK) {
        web_debug_note_hid_interface(info.VID, info.PID, params.addr,
                                     params.iface_num, params.sub_class,
                                     params.proto);
    } else {
        web_debug_note_usb_error("device_info", error);
    }

    if (params.sub_class == HID_SUBCLASS_BOOT_INTERFACE) {
        error = hid_class_request_set_protocol(handle, HID_REPORT_PROTOCOL_BOOT);
        if (error != ESP_OK) {
            web_debug_note_usb_error("set_boot_protocol", error);
            ESP_LOGW(TAG, "set Boot Protocol failed: %s", esp_err_to_name(error));
        }
    }
    if (params.proto == HID_PROTOCOL_KEYBOARD) {
        error = hid_class_request_set_idle(handle, 0, 0);
        if (error != ESP_OK) {
            web_debug_note_usb_error("set_idle", error);
            ESP_LOGW(TAG, "set Idle failed: %s", esp_err_to_name(error));
        }
    }
    error = hid_host_device_start(handle);
    if (error != ESP_OK) {
        web_debug_note_usb_error("device_start", error);
        ESP_LOGE(TAG, "start HID interface failed: %s", esp_err_to_name(error));
        ESP_ERROR_CHECK_WITHOUT_ABORT(hid_host_device_close(handle));
        return;
    }
    if (params.proto == HID_PROTOCOL_KEYBOARD) {
        xSemaphoreTake(s_state_lock, portMAX_DELAY);
        s_usb_keyboard = handle;
        xSemaphoreGive(s_state_lock);
        web_debug_set_keyboard_online(true);
        ESP_LOGI(TAG, "USB keyboard online: VID=%04x PID=%04x addr=%u iface=%u",
                 info.VID, info.PID, params.addr, params.iface_num);
    }
}

static void usb_driver_callback(hid_host_device_handle_t handle,
                                hid_host_driver_event_t event, void *arg)
{
    (void)arg;
    const usb_device_event_t message = {
        .handle = handle,
        .event = event,
    };
    if (xQueueSend(s_usb_event_queue, &message, 0) != pdTRUE) {
        web_debug_note_usb_error("event_queue_full", ESP_ERR_NO_MEM);
    }
}

static bool usb_enumeration_callback(const usb_device_desc_t *descriptor,
                                     uint8_t *configuration_value)
{
    web_debug_note_enumeration(descriptor->idVendor, descriptor->idProduct,
                               descriptor->bDeviceClass,
                               descriptor->bDeviceSubClass,
                               descriptor->bDeviceProtocol);
    *configuration_value = 1;
    return true;
}

static void usb_library_task(void *arg)
{
    TaskHandle_t parent = arg;
    const usb_host_config_t config = {
        .skip_phy_setup = false,
        .intr_flags = ESP_INTR_FLAG_LEVEL1,
        .enum_filter_cb = usb_enumeration_callback,
    };
    ESP_ERROR_CHECK(usb_host_install(&config));
    xTaskNotifyGive(parent);

    while (true) {
        uint32_t flags;
        usb_host_lib_handle_events(portMAX_DELAY, &flags);
    }
}

static void usb_application_task(void *arg)
{
    (void)arg;
    xTaskCreatePinnedToCore(usb_library_task, "usb_events", 4096,
                            xTaskGetCurrentTaskHandle(), 3, NULL, 0);
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(2000));

    const hid_host_driver_config_t config = {
        .create_background_task = true,
        .task_priority = 5,
        .stack_size = 4096,
        .core_id = 0,
        .callback = usb_driver_callback,
        .callback_arg = NULL,
    };
    ESP_ERROR_CHECK(hid_host_install(&config));
    web_debug_set_usb_host_ready(true);
    ESP_LOGI(TAG, "USB Host ready; waiting for HHKB");

    usb_device_event_t message;
    while (xQueueReceive(s_usb_event_queue, &message, portMAX_DELAY) == pdTRUE) {
        if (message.event == HID_HOST_DRIVER_EVENT_CONNECTED) {
            usb_open_device(message.handle);
        }
    }
}

static void bridge_task(void *arg)
{
    (void)arg;
    keyboard_report_t report;
    uint8_t leds;

    while (true) {
        if (xQueueReceive(s_report_queue, &report, pdMS_TO_TICKS(10)) == pdTRUE &&
            atomic_load(&s_ble_connected) && atomic_load(&s_ble_ready)) {
            esp_err_t err = esp_hidd_dev_input_set(s_ble_hid, 0,
                                                   HID_REPORT_ID_KEYBOARD,
                                                   report.bytes,
                                                   sizeof(report.bytes));
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "BLE report failed: %s", esp_err_to_name(err));
            }
        }

        if (xQueueReceive(s_led_queue, &leds, 0) == pdTRUE) {
            xSemaphoreTake(s_state_lock, portMAX_DELAY);
            hid_host_device_handle_t keyboard = s_usb_keyboard;
            xSemaphoreGive(s_state_lock);
            if (keyboard != NULL) {
                ESP_ERROR_CHECK_WITHOUT_ABORT(hid_class_request_set_report(
                    keyboard, HID_REPORT_TYPE_OUTPUT, 0, &leds, 1));
            }
        }
    }
}

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    s_usb_event_queue = xQueueCreate(USB_EVENT_QUEUE_DEPTH, sizeof(usb_device_event_t));
    s_report_queue = xQueueCreate(REPORT_QUEUE_DEPTH, sizeof(keyboard_report_t));
    s_led_queue = xQueueCreate(1, sizeof(uint8_t));
    s_state_lock = xSemaphoreCreateMutex();
    assert(s_usb_event_queue && s_report_queue && s_led_queue && s_state_lock);

    ble_init();
    xTaskCreatePinnedToCore(usb_application_task, "usb_app", 6144, NULL, 4, NULL, 0);
    xTaskCreatePinnedToCore(bridge_task, "hid_bridge", 4096, NULL, 5, NULL, 1);
    ESP_ERROR_CHECK(web_debug_start());
}
