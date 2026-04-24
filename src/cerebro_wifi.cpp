// ═══════════════════════════════════════════════════════════
//  CEREBRO — WiFi Server Module (mDNS + HTTP API)
//  Replaces BLE audio/command transport with local network HTTP
// ═══════════════════════════════════════════════════════════

#include "cerebro_wifi.h"
#include "cerebro_audio.h"
#include "cerebro_ble.h"
#include "config.h"

#include <WiFi.h>
#include <ESPmDNS.h>
#include <WebServer.h>

static WebServer server(80);
static bool serverRunning = false;
static unsigned long lastAppPing = 0;
#define APP_TIMEOUT_MS 30000

// Face code set by app via HTTP
static volatile int8_t httpFaceCode = -1;

// ── Helpers ────────────────────────────────────────────────

static void cors() {
    server.sendHeader("Access-Control-Allow-Origin", "*");
    server.sendHeader("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
    server.sendHeader("Access-Control-Allow-Headers", "Content-Type");
}

static void touch() { lastAppPing = millis(); }

// ── GET /info ──────────────────────────────────────────────
// Device identification (used after mDNS discovery)
static void handleInfo() {
    touch();
    cors();
    char json[256];
    snprintf(json, sizeof(json),
        "{\"name\":\"CEREBRO\",\"ip\":\"%s\",\"mac\":\"%s\",\"version\":\"1.0\"}",
        WiFi.localIP().toString().c_str(),
        WiFi.macAddress().c_str());
    server.send(200, "application/json", json);
}

// ── GET /status ────────────────────────────────────────────
// Full device state (poll this for live updates)
static void handleStatus() {
    touch();
    cors();
    AudioState as = audioGetState();
    const char *audioStr = "idle";
    if (as == AUDIO_RECORDING) audioStr = "recording";
    else if (as == AUDIO_PLAYING) audioStr = "playing";

    char json[384];
    snprintf(json, sizeof(json),
        "{\"audio\":\"%s\","
        "\"mic\":%s,"
        "\"speaker\":%s,"
        "\"amplitude\":%d,"
        "\"recording_bytes\":%d,"
        "\"battery\":%d,"
        "\"wifi\":true,"
        "\"face\":%d}",
        audioStr,
        audioIsMicActive() ? "true" : "false",
        audioIsSpeakerActive() ? "true" : "false",
        audioGetSpeakerAmplitude(),
        audioGetRecordingLength(),
        -1,  // battery filled in by main if available
        httpFaceCode);
    server.send(200, "application/json", json);
}

// ── POST /command ──────────────────────────────────────────
// Commands: START_MIC, STOP_MIC, PLAY:<url>, PING
static String getBody() {
    // Try "plain" (text/plain), then all numbered args, then named "cmd"
    String body = server.arg("plain");
    if (body.length() == 0 && server.args() > 0) body = server.arg(0);
    if (body.length() == 0) body = server.arg("cmd");
    body.trim();
    // If body looks like JSON {"cmd":"X"}, extract X
    int idx = body.indexOf("\"cmd\"");
    if (idx >= 0) {
        int colon = body.indexOf(':', idx);
        int q1 = body.indexOf('"', colon + 1);
        int q2 = body.indexOf('"', q1 + 1);
        if (q1 >= 0 && q2 > q1) body = body.substring(q1 + 1, q2);
    }
    return body;
}

static void handleCommand() {
    touch();
    cors();
    String cmd = getBody();
    Serial.printf("[HTTP] Cmd: '%s'\n", cmd.c_str());

    if (cmd == "START_MIC") {
        audioStartRecording();
        server.send(200, "application/json", "{\"ok\":true,\"status\":\"recording\"}");
    } else if (cmd == "STOP_MIC") {
        audioStopRecording();
        char resp[64];
        snprintf(resp, sizeof(resp), "{\"ok\":true,\"bytes\":%d}", audioGetRecordingLength());
        server.send(200, "application/json", resp);
    } else if (cmd.startsWith("PLAY:")) {
        String url = cmd.substring(5);
        audioPlayUrl(url.c_str());
        server.send(200, "application/json", "{\"ok\":true,\"status\":\"playing\"}");
    } else if (cmd == "START_SPEAK") {
        audioStartSpeaker();
        server.send(200, "application/json", "{\"ok\":true,\"status\":\"speaker_on\"}");
    } else if (cmd == "STOP_SPEAK") {
        audioStopSpeaker();
        server.send(200, "application/json", "{\"ok\":true,\"status\":\"speaker_off\"}");
    } else if (cmd == "PING") {
        server.send(200, "application/json", "{\"ok\":true,\"status\":\"pong\"}");
    } else {
        server.send(400, "application/json", "{\"ok\":false,\"error\":\"unknown command\"}");
    }
}

// ── POST /face ─────────────────────────────────────────────
// Set face expression: body is single byte value or {"code": N}
static void handleFace() {
    touch();
    cors();
    String body = server.arg("plain");
    body.trim();

    int code = -1;
    // Try simple number
    if (body.length() > 0 && body[0] >= '0' && body[0] <= '9') {
        code = body.toInt();
    }
    // Try JSON {"code": N}
    int idx = body.indexOf("\"code\"");
    if (idx >= 0) {
        int colon = body.indexOf(':', idx);
        if (colon >= 0) {
            String val = body.substring(colon + 1);
            val.trim();
            code = val.toInt();
        }
    }

    if (code >= 0 && code <= 0x0A) {
        httpFaceCode = (int8_t)code;
        Serial.printf("[HTTP] Face: 0x%02X\n", code);
        server.send(200, "application/json", "{\"ok\":true}");
    } else {
        server.send(400, "application/json", "{\"ok\":false,\"error\":\"invalid code (0-10)\"}");
    }
}

// ── POST /audio ────────────────────────────────────────────
// Push raw PCM data to speaker ring buffer
static void handleAudio() {
    touch();
    cors();
    if (!server.hasArg("plain") || server.arg("plain").length() == 0) {
        // Check raw body
        WiFiClient &client = server.client();
        // Body already in arg("plain") for POST
    }

    const String &body = server.arg("plain");
    size_t len = body.length();
    if (len > 0) {
        audioSpeakerPush((const uint8_t *)body.c_str(), len);
        char resp[64];
        snprintf(resp, sizeof(resp), "{\"ok\":true,\"bytes\":%d}", len);
        server.send(200, "application/json", resp);
    } else {
        server.send(400, "application/json", "{\"ok\":false,\"error\":\"no data\"}");
    }
}

// ── GET /recording ─────────────────────────────────────────
// Download last mic recording as WAV
static void handleRecording() {
    touch();
    cors();
    size_t len = 0;
    const uint8_t *data = audioGetRecording(&len);

    if (!data || len == 0) {
        server.send(204, "application/json", "{\"ok\":false,\"error\":\"no recording\"}");
        return;
    }

    // Build WAV header
    uint8_t wav[44];
    uint32_t fileSize = len + 36;
    uint32_t byteRate = SAMPLE_RATE * 1 * (BITS_PER_SAMPLE / 8);
    uint16_t blockAlign = 1 * (BITS_PER_SAMPLE / 8);

    memcpy(wav, "RIFF", 4);
    memcpy(wav + 4, &fileSize, 4);
    memcpy(wav + 8, "WAVE", 4);
    memcpy(wav + 12, "fmt ", 4);
    uint32_t fmtSize = 16;
    memcpy(wav + 16, &fmtSize, 4);
    uint16_t audioFmt = 1; // PCM
    memcpy(wav + 20, &audioFmt, 2);
    uint16_t channels = 1;
    memcpy(wav + 22, &channels, 2);
    uint32_t sr = SAMPLE_RATE;
    memcpy(wav + 24, &sr, 4);
    memcpy(wav + 28, &byteRate, 4);
    memcpy(wav + 32, &blockAlign, 2);
    uint16_t bps = BITS_PER_SAMPLE;
    memcpy(wav + 34, &bps, 2);
    memcpy(wav + 36, "data", 4);
    uint32_t dataSize = len;
    memcpy(wav + 40, &dataSize, 4);

    // Stream response
    WiFiClient &client = server.client();
    server.setContentLength(44 + len);
    server.sendHeader("Content-Disposition", "attachment; filename=\"recording.wav\"");
    server.send(200, "audio/wav", "");
    client.write(wav, 44);

    // Send PCM data in chunks
    size_t sent = 0;
    while (sent < len) {
        size_t chunk = min((size_t)4096, len - sent);
        client.write(data + sent, chunk);
        sent += chunk;
    }

    Serial.printf("[HTTP] Sent recording: %d bytes WAV\n", 44 + len);
}

// ── OPTIONS (CORS preflight) ───────────────────────────────
static void handleOptions() {
    cors();
    server.send(204);
}

// ── Public API ─────────────────────────────────────────────

void wifiServerInit() {
    // Nothing to allocate here — recording buffer is in audio module
    Serial.println("[HTTP] Server module ready");
}

void wifiServerStart() {
    if (serverRunning) return;
    if (WiFi.status() != WL_CONNECTED) return;

    // mDNS — makes device discoverable as "cerebro.local"
    if (MDNS.begin("cerebro")) {
        MDNS.addService("cerebro", "tcp", 80);
        Serial.printf("[MDNS] Advertising _cerebro._tcp on %s:80\n",
                      WiFi.localIP().toString().c_str());
    } else {
        Serial.println("[MDNS] Failed to start");
    }

    // HTTP routes
    server.on("/info", HTTP_GET, handleInfo);
    server.on("/status", HTTP_GET, handleStatus);
    server.on("/command", HTTP_POST, handleCommand);
    server.on("/face", HTTP_POST, handleFace);
    server.on("/audio", HTTP_POST, handleAudio);
    server.on("/recording", HTTP_GET, handleRecording);

    // CORS preflight for all routes
    server.on("/info", HTTP_OPTIONS, handleOptions);
    server.on("/status", HTTP_OPTIONS, handleOptions);
    server.on("/command", HTTP_OPTIONS, handleOptions);
    server.on("/face", HTTP_OPTIONS, handleOptions);
    server.on("/audio", HTTP_OPTIONS, handleOptions);
    server.on("/recording", HTTP_OPTIONS, handleOptions);

    server.begin();
    serverRunning = true;
    Serial.printf("[HTTP] Server started on port 80\n");
}

void wifiServerLoop() {
    if (!serverRunning) {
        // Auto-start when WiFi connects
        if (bleWifiConnected()) wifiServerStart();
        return;
    }
    server.handleClient();
}

bool wifiAppConnected() {
    if (lastAppPing == 0) return false;
    return (millis() - lastAppPing) < APP_TIMEOUT_MS;
}

int8_t wifiGetFaceCode() { return httpFaceCode; }
