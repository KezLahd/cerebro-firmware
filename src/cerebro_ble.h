#pragma once
#include <Arduino.h>

// BLE connection states
enum BleState { BLE_OFF, BLE_ADVERTISING, BLE_CONNECTED, BLE_PAIRED };

// Status strings (pairing flow only)
#define STATUS_READY          "READY"
#define STATUS_WIFI_CONNECTING "WIFI_CONNECTING"
#define STATUS_WIFI_CONNECTED "WIFI_CONNECTED"
#define STATUS_WIFI_FAILED    "WIFI_FAILED"

void bleInit();
void bleLoop();
void bleSendStatus(const char *status);
void bleSendAudioOut(const uint8_t *data, size_t len);
BleState bleGetState();
bool bleWifiConnected();
const char* bleGetServer();
bool bleIsConnected();
