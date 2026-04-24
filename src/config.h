#pragma once

// ═══════════════════════════════════════════════════════════
//  CEREBRO — Configuration
// ═══════════════════════════════════════════════════════════

// ── BLE Pairing ──────────────────────────────────────────
// Custom GATT service for Cerebro phone app pairing
// UUIDs must be valid 128-bit hex (8-4-4-4-12 format)
#define BLE_SERVICE_UUID        "ce3eb000-b001-4000-8000-000000000000"
#define BLE_CHAR_SSID_UUID      "ce3eb000-b001-4000-8000-000000000001"
#define BLE_CHAR_PASS_UUID      "ce3eb000-b001-4000-8000-000000000002"
#define BLE_CHAR_SERVER_UUID    "ce3eb000-b001-4000-8000-000000000003"
#define BLE_CHAR_COMMAND_UUID   "ce3eb000-b001-4000-8000-000000000004"
#define BLE_CHAR_STATUS_UUID    "ce3eb000-b001-4000-8000-000000000005"

// ── NVS Storage ──────────────────────────────────────────
#define NVS_NAMESPACE   "cerebro"
#define NVS_KEY_SSID    "wifi_ssid"
#define NVS_KEY_PASS    "wifi_pass"
#define NVS_KEY_SERVER  "server"
#define NVS_KEY_PAIRED  "paired"

// ── Wi-Fi ────────────────────────────────────────────────
#define WIFI_TIMEOUT_MS 10000

// ── Display (CO5300 QSPI) ────────────────────────────────
#define PIN_LCD_CS      12
#define PIN_LCD_SCLK    38
#define PIN_LCD_D0      4
#define PIN_LCD_D1      5
#define PIN_LCD_D2      6
#define PIN_LCD_D3      7
#define PIN_LCD_RST     2

#define SCREEN_W        466
#define SCREEN_H        466

// ── BOOT Button ──────────────────────────────────────────
#define PIN_BOOT_BTN    0       // GPIO0, active LOW
#define BOOT_HOLD_MS    2000    // Hold 2 seconds to enter pairing

// ── I2C Bus (shared) ─────────────────────────────────────
#define PIN_I2C_SDA     15
#define PIN_I2C_SCL     14

// ── I2S Audio ────────────────────────────────────────────
#define PIN_I2S_MCLK    16
#define PIN_I2S_BCLK    9
#define PIN_I2S_LRCK    45
#define PIN_I2S_DOUT    8   // ESP32 → ES8311 (speaker)
#define PIN_I2S_DIN     10  // ES7210 → ESP32 (mic)
#define PIN_SPEAKER_EN  46

// ── Touch (CST9217) ─────────────────────────────────────
#define PIN_TOUCH_INT   11
#define PIN_TOUCH_RST   2   // shared with LCD_RST on 1.75C

// ── I2C Addresses ────────────────────────────────────────
#define AXP2101_ADDR    0x34
#define ES7210_ADDR     0x40
#define ES8311_ADDR     0x18

// ── Audio Config ─────────────────────────────────────────
#define SAMPLE_RATE     8000
#define BITS_PER_SAMPLE 16
#define NUM_CHANNELS    1
#define I2S_READ_SIZE   1024

// Audio buffer: 2MB in PSRAM (~65 seconds at 16kHz/16bit/mono)
#define WAV_HEADER_SIZE 44
#define MAX_AUDIO_SIZE  (2 * 1024 * 1024)
#define MAX_RECORD_MS   65000

// ── Colours ──────────────────────────────────────────────
#define CEREBRO_RED_R   196
#define CEREBRO_RED_G   30
#define CEREBRO_RED_B   58

#define CEREBRO_GREEN_R 76
#define CEREBRO_GREEN_G 175
#define CEREBRO_GREEN_B 80

#define CEREBRO_BLUE_R  66
#define CEREBRO_BLUE_G  133
#define CEREBRO_BLUE_B  244
