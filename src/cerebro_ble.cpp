// ═══════════════════════════════════════════════════════════
//  CEREBRO — BLE Module (pairing only)
//  After WiFi pairing, all comms go via HTTP (cerebro_wifi.cpp)
// ═══════════════════════════════════════════════════════════

#include "cerebro_ble.h"
#include "config.h"

#include <NimBLEDevice.h>
#include <WiFi.h>
#include <Preferences.h>

// ── State ───────────────────────────────────────────────────
static BleState state = BLE_OFF;
static NimBLEServer *pServer = nullptr;
static NimBLECharacteristic *pStatusChar = nullptr;
static NimBLECharacteristic *pAudioOutChar = nullptr;
static Preferences prefs;

static String rxSsid, rxPass, rxServer;
static bool wifiConnected = false;
static bool wifiConnectRequested = false;
static char serverAddr[64] = {0};
static bool phoneConnected = false;

// ── Wi-Fi ───────────────────────────────────────────────────
static void connectWifi() {
    if (rxSsid.length() == 0) { bleSendStatus("ERROR:No SSID"); return; }
    Serial.printf("[WIFI] Connecting to '%s'...\n", rxSsid.c_str());
    bleSendStatus(STATUS_WIFI_CONNECTING);
    WiFi.mode(WIFI_STA);
    WiFi.begin(rxSsid.c_str(), rxPass.c_str());
    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < WIFI_TIMEOUT_MS) delay(100);
    if (WiFi.status() == WL_CONNECTED) {
        wifiConnected = true;
        state = BLE_PAIRED;
        prefs.begin(NVS_NAMESPACE, false);
        prefs.putString(NVS_KEY_SSID, rxSsid);
        prefs.putString(NVS_KEY_PASS, rxPass);
        prefs.putString(NVS_KEY_SERVER, rxServer);
        prefs.putBool(NVS_KEY_PAIRED, true);
        prefs.end();
        strncpy(serverAddr, rxServer.c_str(), sizeof(serverAddr)-1);
        Serial.printf("[WIFI] Connected! IP: %s\n", WiFi.localIP().toString().c_str());
        bleSendStatus(STATUS_WIFI_CONNECTED);
    } else {
        wifiConnected = false;
        Serial.println("[WIFI] Failed");
        bleSendStatus(STATUS_WIFI_FAILED);
    }
}

static bool tryStoredWifi() {
    prefs.begin(NVS_NAMESPACE, true);
    bool paired = prefs.getBool(NVS_KEY_PAIRED, false);
    if (!paired) { prefs.end(); return false; }
    String ssid = prefs.getString(NVS_KEY_SSID, "");
    String pass = prefs.getString(NVS_KEY_PASS, "");
    String server = prefs.getString(NVS_KEY_SERVER, "");
    prefs.end();
    if (ssid.length() == 0) return false;
    Serial.printf("[WIFI] Reconnecting to '%s'...\n", ssid.c_str());
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid.c_str(), pass.c_str());
    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < WIFI_TIMEOUT_MS) delay(100);
    if (WiFi.status() == WL_CONNECTED) {
        wifiConnected = true;
        strncpy(serverAddr, server.c_str(), sizeof(serverAddr)-1);
        Serial.printf("[WIFI] Reconnected! IP: %s\n", WiFi.localIP().toString().c_str());
        return true;
    }
    return false;
}

// ── BLE Callbacks ───────────────────────────────────────────
class ServerCallbacks : public NimBLEServerCallbacks {
    void onConnect(NimBLEServer *s) override {
        state = BLE_CONNECTED;
        phoneConnected = true;
        Serial.println("[BLE] Connected");
        NimBLEDevice::getAdvertising()->stop();
    }
    void onDisconnect(NimBLEServer *s) override {
        phoneConnected = false;
        Serial.println("[BLE] Disconnected");
        state = wifiConnected ? BLE_PAIRED : BLE_ADVERTISING;
        NimBLEDevice::getAdvertising()->start();
    }
};

class CharCallbacks : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic *pChar) override {
        std::string uuid = pChar->getUUID().toString();
        std::string val = pChar->getValue();

        // ── Wi-Fi provisioning ──────────────────────
        if (uuid == BLE_CHAR_SSID_UUID) {
            rxSsid = String(val.c_str());
        } else if (uuid == BLE_CHAR_PASS_UUID) {
            rxPass = String(val.c_str());
        } else if (uuid == BLE_CHAR_SERVER_UUID) {
            rxServer = String(val.c_str());

        // ── Commands (pairing only) ─────────────────
        } else if (uuid == BLE_CHAR_COMMAND_UUID) {
            String cmd = String(val.c_str());
            Serial.printf("[BLE] Cmd: %s\n", cmd.c_str());

            if (cmd == "CONNECT_WIFI") {
                wifiConnectRequested = true;
            } else if (cmd == "DISCONNECT") {
                WiFi.disconnect(); wifiConnected = false;
                prefs.begin(NVS_NAMESPACE, false);
                prefs.putBool(NVS_KEY_PAIRED, false);
                prefs.end();
                bleSendStatus(STATUS_READY);
            }
        }
    }
};

static ServerCallbacks serverCB;
static CharCallbacks charCB;

// ── Public API ──────────────────────────────────────────────

void bleInit() {
    Serial.println("[BLE] Init...");
    if (tryStoredWifi()) state = BLE_PAIRED;

    NimBLEDevice::init("CEREBRO");
    NimBLEDevice::setPower(ESP_PWR_LVL_P9);
    NimBLEDevice::setMTU(517);

    pServer = NimBLEDevice::createServer();
    pServer->setCallbacks(&serverCB);

    NimBLEService *svc = pServer->createService(BLE_SERVICE_UUID);

    // ── Pairing characteristics ────────────────────
    auto *pSsid = svc->createCharacteristic(BLE_CHAR_SSID_UUID, NIMBLE_PROPERTY::WRITE);
    pSsid->setCallbacks(&charCB);

    auto *pPass = svc->createCharacteristic(BLE_CHAR_PASS_UUID, NIMBLE_PROPERTY::WRITE);
    pPass->setCallbacks(&charCB);

    auto *pSrv = svc->createCharacteristic(BLE_CHAR_SERVER_UUID, NIMBLE_PROPERTY::WRITE);
    pSrv->setCallbacks(&charCB);

    auto *pCmd = svc->createCharacteristic(BLE_CHAR_COMMAND_UUID, NIMBLE_PROPERTY::WRITE);
    pCmd->setCallbacks(&charCB);

    pStatusChar = svc->createCharacteristic(BLE_CHAR_STATUS_UUID, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);
    pStatusChar->setValue((const uint8_t *)STATUS_READY, strlen(STATUS_READY));

    // Audio out kept for legacy BLE mic streaming during pairing test
    pAudioOutChar = svc->createCharacteristic(
        "ce3eb000-b001-4000-8000-000000000010", NIMBLE_PROPERTY::NOTIFY);

    svc->start();

    NimBLEAdvertising *adv = NimBLEDevice::getAdvertising();
    adv->addServiceUUID(BLE_SERVICE_UUID);
    adv->setName("CEREBRO");
    adv->setScanResponse(true);
    adv->start();

    state = wifiConnected ? BLE_PAIRED : BLE_ADVERTISING;
    Serial.println("[BLE] Ready");
}

void bleLoop() {
    if (wifiConnectRequested) { wifiConnectRequested = false; connectWifi(); }

    // Wi-Fi health check
    static unsigned long lastCheck = 0;
    if (wifiConnected && millis() - lastCheck > 10000) {
        lastCheck = millis();
        if (WiFi.status() != WL_CONNECTED) {
            wifiConnected = false;
            WiFi.reconnect();
            unsigned long s = millis();
            while (WiFi.status() != WL_CONNECTED && millis() - s < WIFI_TIMEOUT_MS) delay(100);
            wifiConnected = (WiFi.status() == WL_CONNECTED);
        }
    }
}

void bleSendStatus(const char *status) {
    if (pStatusChar && phoneConnected) {
        pStatusChar->setValue((const uint8_t *)status, strlen(status));
        pStatusChar->notify();
    }
}

void bleSendAudioOut(const uint8_t *data, size_t len) {
    if (pAudioOutChar && phoneConnected) {
        pAudioOutChar->setValue(data, len);
        pAudioOutChar->notify();
    }
}

BleState bleGetState() { return state; }
bool bleWifiConnected() { return wifiConnected; }
const char* bleGetServer() { return serverAddr; }
bool bleIsConnected() { return phoneConnected; }
