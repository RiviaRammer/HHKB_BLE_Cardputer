#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

esp_err_t web_debug_start(void);
void web_debug_set_usb_host_ready(bool ready);
void web_debug_set_keyboard_online(bool online);
void web_debug_set_ble_connected(bool connected);
void web_debug_set_ble_ready(bool ready);
void web_debug_note_hid_interface(uint16_t vid, uint16_t pid, uint8_t address,
                                  uint8_t interface_number, uint8_t subclass,
                                  uint8_t protocol);
void web_debug_note_enumeration(uint16_t vid, uint16_t pid, uint8_t device_class,
                                uint8_t subclass, uint8_t protocol);
void web_debug_note_usb_error(const char *stage, esp_err_t error);
void web_debug_record_report(const uint8_t *data, size_t length);
