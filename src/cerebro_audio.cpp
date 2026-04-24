// ═══════════════════════════════════════════════════════════
//  CEREBRO — Audio Module (BLE streaming)
//  Single I2S port, stereo full-duplex
//  Speaker: mono→stereo, Mic: stereo→mono (CH0)
// ═══════════════════════════════════════════════════════════

#include "cerebro_audio.h"
#include "cerebro_ble.h"
#include "config.h"
#include "es8311.h"
#include "es7210.h"

#include <Wire.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <driver/i2s.h>

#define I2S_PORT I2S_NUM_0

// ── Ring Buffer ─────────────────────────────────────────────
#define RING_BUF_SIZE (256 * 1024)  // 256KB in PSRAM — buffer entire TTS response
static uint8_t *ringBuf = nullptr;
static volatile size_t ringHead = 0;
static volatile size_t ringTail = 0;

static size_t ringAvailable() { return (ringHead - ringTail + RING_BUF_SIZE) % RING_BUF_SIZE; }
static size_t ringFree() { return RING_BUF_SIZE - 1 - ringAvailable(); }
static void ringWrite(const uint8_t *data, size_t len) {
    for (size_t i = 0; i < len; i++) {
        ringBuf[ringHead] = data[i];
        ringHead = (ringHead + 1) % RING_BUF_SIZE;
    }
}
static size_t ringRead(uint8_t *data, size_t maxLen) {
    size_t avail = ringAvailable();
    size_t toRead = min(avail, maxLen);
    for (size_t i = 0; i < toRead; i++) {
        data[i] = ringBuf[ringTail];
        ringTail = (ringTail + 1) % RING_BUF_SIZE;
    }
    return toRead;
}

// ── State ───────────────────────────────────────────────────
static volatile AudioState state = AUDIO_IDLE;
static volatile bool micActive = false;
static volatile bool speakerActive = false;
static volatile bool speakerDraining = false;
static volatile bool speakerPlaying = false;
static volatile uint8_t speakerAmplitude = 0;

// WiFi playback
static char playbackUrl[256] = {0};
static volatile bool wifiPlayRequested = false;
#define PREBUFFER_HIGH 48000   // 3 seconds before starting
#define PREBUFFER_LOW  0       // Never pause

// ── Recording Buffer (PSRAM) ────────────────────────────────
static uint8_t *recBuf = nullptr;
static volatile size_t recLen = 0;
static volatile bool recActive = false;

// ── Mic amplitude + silence detection ───────────────────────
static volatile uint8_t micAmplitude = 0;
static unsigned long lastLoudTime = 0;
#define SILENCE_THRESHOLD 10    // amplitude below this = silence (noise floor ~7)
#define SILENCE_TIMEOUT_MS 1500 // auto-stop after 1.5s of silence
#define MIN_RECORD_MS 1000      // don't auto-stop in first 1s

// ── I2S Init ────────────────────────────────────────────────
static bool initI2S() {
    i2s_config_t cfg = {};
    cfg.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX | I2S_MODE_RX);
    cfg.sample_rate = SAMPLE_RATE;
    cfg.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
    cfg.channel_format = I2S_CHANNEL_FMT_ONLY_LEFT; // Mono
    cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
    cfg.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
    cfg.dma_buf_count = 8;
    cfg.dma_buf_len = 256;
    cfg.use_apll = true;
    cfg.tx_desc_auto_clear = true;
    cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;

    if (i2s_driver_install(I2S_PORT, &cfg, 0, NULL) != ESP_OK) return false;
    i2s_pin_config_t pins = {};
    pins.mck_io_num = PIN_I2S_MCLK;
    pins.bck_io_num = PIN_I2S_BCLK;
    pins.ws_io_num = PIN_I2S_LRCK;
    pins.data_out_num = PIN_I2S_DOUT;
    pins.data_in_num = PIN_I2S_DIN;
    if (i2s_set_pin(I2S_PORT, &pins) != ESP_OK) return false;
    i2s_zero_dma_buffer(I2S_PORT);
    Serial.println("[AUDIO] I2S stereo full-duplex OK");
    return true;
}

// ── Codec Init ──────────────────────────────────────────────
static bool initES8311() {
    es8311_handle_t h = es8311_create(0, ES8311_ADDR);
    if (!h) return false;
    es8311_clock_config_t clk = {
        .mclk_inverted = false, .sclk_inverted = false,
        .mclk_from_mclk_pin = true,
        .mclk_frequency = SAMPLE_RATE * 256,
        .sample_frequency = SAMPLE_RATE
    };
    if (es8311_init(h, &clk, ES8311_RESOLUTION_16, ES8311_RESOLUTION_16) != ESP_OK) return false;
    es8311_sample_frequency_config(h, clk.mclk_frequency, clk.sample_frequency);
    es8311_voice_volume_set(h, 90, NULL);
    es8311_microphone_config(h, false);
    Serial.println("[AUDIO] ES8311 OK");
    return true;
}

static bool initES7210() {
    audio_hal_codec_config_t cfg = {
        .adc_input = AUDIO_HAL_ADC_INPUT_ALL,
        .dac_output = AUDIO_HAL_DAC_OUTPUT_ALL,
        .codec_mode = AUDIO_HAL_CODEC_MODE_ENCODE,
        .i2s_iface = {
            .mode = AUDIO_HAL_MODE_SLAVE,
            .fmt = AUDIO_HAL_I2S_NORMAL,
            .samples = AUDIO_HAL_08K_SAMPLES,
            .bits = AUDIO_HAL_BIT_LENGTH_16BITS,
        }
    };
    if (es7210_adc_init(&Wire, &cfg) != ESP_OK) return false;
    es7210_adc_config_i2s(cfg.codec_mode, &cfg.i2s_iface);
    es7210_adc_set_gain(
        (es7210_input_mics_t)(ES7210_INPUT_MIC1 | ES7210_INPUT_MIC2),
        GAIN_33DB);
    es7210_adc_ctrl_state(cfg.codec_mode, AUDIO_HAL_CTRL_START);
    Serial.println("[AUDIO] ES7210 OK");
    return true;
}

// ── Keep TX alive with silence (prevents DMA stall) ─────────
static int16_t silenceBuf[256] = {0};  // Mono silence

static void feedSilence() {
    size_t bw;
    i2s_write(I2S_PORT, silenceBuf, sizeof(silenceBuf), &bw, 0);
}

// ── WiFi Audio Playback ─────────────────────────────────────
static void wifiPlayback() {
    Serial.printf("[AUDIO] WiFi playback: %s\n", playbackUrl);
    state = AUDIO_PLAYING;
    speakerActive = true;

    bool isHttps = strncmp(playbackUrl, "https", 5) == 0;
    WiFiClientSecure secureClient;
    WiFiClient plainClient;

    if (isHttps) {
        secureClient.setInsecure();  // skip cert verification
        Serial.println("[AUDIO] Using HTTPS (insecure)");
    }

    HTTPClient http;
    if (isHttps) {
        http.begin(secureClient, playbackUrl);
    } else {
        http.begin(plainClient, playbackUrl);
    }
    http.setTimeout(15000);
    Serial.println("[AUDIO] Fetching...");
    int code = http.GET();
    Serial.printf("[AUDIO] HTTP %d\n", code);

    if (code != 200) {
        Serial.printf("[AUDIO] HTTP GET failed: %d\n", code);
        speakerActive = false;
        state = AUDIO_IDLE;
        http.end();
        return;
    }

    WiFiClient *stream = http.getStreamPtr();
    int contentLen = http.getSize();
    Serial.printf("[AUDIO] Content-Length: %d bytes\n", contentLen);

    // Parse WAV header if present — need sample rate for I2S
    uint32_t wavSampleRate = SAMPLE_RATE;
    uint32_t wavDataSize = 0;
    uint8_t hdr[44];
    bool hasWavHeader = false;

    if (contentLen == -1 || contentLen > 44) {
        size_t rd = stream->readBytes(hdr, 44);
        Serial.printf("[AUDIO] Header bytes read: %d\n", rd);
        if (rd == 44 && memcmp(hdr, "RIFF", 4) == 0 && memcmp(hdr + 8, "WAVE", 4) == 0) {
            hasWavHeader = true;
            memcpy(&wavSampleRate, hdr + 24, 4);
            memcpy(&wavDataSize, hdr + 40, 4);
            uint16_t wavChannels = 0, wavBits = 0;
            memcpy(&wavChannels, hdr + 22, 2);
            memcpy(&wavBits, hdr + 34, 2);
            Serial.printf("[AUDIO] WAV: %dHz %dch %dbit data=%d bytes\n",
                          wavSampleRate, wavChannels, wavBits, wavDataSize);
        } else if (rd == 44) {
            Serial.println("[AUDIO] Not WAV, playing raw");
            size_t bw;
            i2s_write(I2S_PORT, hdr, 44, &bw, portMAX_DELAY);
        }
    }

    // Switch I2S sample rate if WAV differs from default
    bool rateChanged = false;
    if (wavSampleRate != SAMPLE_RATE) {
        i2s_set_sample_rates(I2S_PORT, wavSampleRate);
        rateChanged = true;
        Serial.printf("[AUDIO] I2S rate → %d Hz\n", wavSampleRate);
    }

    // Stream PCM directly to I2S
    uint8_t buf[1024];
    size_t totalRead = 0, totalWritten = 0;
    while (stream->connected() || stream->available()) {
        int avail = stream->available();
        if (avail <= 0) { vTaskDelay(1); continue; }
        int toRead = min(avail, (int)sizeof(buf));
        int rd = stream->readBytes(buf, toRead);
        if (rd > 0) {
            totalRead += rd;

            // Amplitude for mouth animation
            int16_t *ptr = (int16_t *)buf;
            size_t ns = rd / 2;
            int32_t sum = 0;
            for (size_t i = 0; i < ns; i++) sum += abs(ptr[i]);
            speakerAmplitude = min(255, (int)(sum / max((size_t)1, ns) / 128));

            size_t bw;
            i2s_write(I2S_PORT, buf, rd, &bw, portMAX_DELAY);
            totalWritten += bw;
        }
    }

    Serial.printf("[AUDIO] Done: read=%d written=%d\n", totalRead, totalWritten);

    // Restore I2S to default rate for mic recording
    if (rateChanged) {
        i2s_set_sample_rates(I2S_PORT, SAMPLE_RATE);
        Serial.printf("[AUDIO] I2S rate → %d Hz (restored)\n", SAMPLE_RATE);
    }

    speakerAmplitude = 0;
    speakerActive = false;
    http.end();
    state = AUDIO_IDLE;
    Serial.println("[AUDIO] WiFi playback done");
}

// ── Audio Task (Core 0) ─────────────────────────────────────
static void audioTask(void *param) {
    uint8_t micBuf[480];
    uint8_t spkBuf[480];

    while (true) {
        // ── WiFi playback request ───────────────────────
        if (wifiPlayRequested) {
            wifiPlayRequested = false;
            wifiPlayback();
            continue;
        }

        // ── Mic (mono read, record to buffer or stream via BLE) ──
        if (micActive) {
            size_t bytesRead = 0;
            esp_err_t err = i2s_read(I2S_PORT, micBuf, 480, &bytesRead, 100 / portTICK_PERIOD_MS);
            if (err == ESP_OK && bytesRead > 0) {
                int16_t *samples = (int16_t *)micBuf;
                size_t numSamples = bytesRead / 2;
                static int32_t dcOffset = 0;

                int32_t sum = 0;
                for (size_t i = 0; i < numSamples; i++) {
                    int32_t s = (int32_t)samples[i];
                    dcOffset += (s - dcOffset) >> 8;
                    s -= dcOffset;
                    s *= 8;
                    if (s > 32767) s = 32767;
                    if (s < -32768) s = -32768;
                    samples[i] = (int16_t)s;
                    sum += abs(s);
                }

                // Update mic amplitude for /status endpoint
                micAmplitude = min(255, (int)(sum / max((size_t)1, numSamples) / 128));

                // Write to recording buffer (WiFi mode)
                if (recActive && recBuf && recLen + bytesRead <= MAX_AUDIO_SIZE) {
                    memcpy(recBuf + recLen, micBuf, bytesRead);
                    recLen += bytesRead;

                    // Silence detection — auto-stop after 1.5s quiet
                    unsigned long now = millis();
                    if (micAmplitude >= SILENCE_THRESHOLD) {
                        lastLoudTime = now;
                    } else if (recLen > (SAMPLE_RATE * 2 * MIN_RECORD_MS / 1000) &&
                               (now - lastLoudTime) >= SILENCE_TIMEOUT_MS) {
                        Serial.println("[AUDIO] Silence detected, auto-stopping");
                        recActive = false;
                        micActive = false;
                        micAmplitude = 0;
                        state = AUDIO_IDLE;
                    }
                }

                // Also send via BLE if phone is connected (legacy/pairing mode)
                bleSendAudioOut(micBuf, bytesRead);
            }

            if (!speakerActive && !speakerDraining) {
                feedSilence();
            }
        }

        // ── Speaker (mono from ring buf → stereo to I2S) ─────
        if (speakerActive || speakerDraining) {
            size_t avail = ringAvailable();

            if (!speakerPlaying) {
                if (avail >= PREBUFFER_HIGH || speakerDraining) {
                    speakerPlaying = true;
                    Serial.printf("[AUDIO] Playing (%d bytes buffered)\n", avail);
                }
                feedSilence();
                vTaskDelay(5 / portTICK_PERIOD_MS);
            } else if (avail >= 2) {
                size_t toRead = min(avail, (size_t)480);
                toRead &= ~1;
                size_t bytes = ringRead(spkBuf, toRead);

                int16_t *ptr = (int16_t *)spkBuf;
                size_t numSamples = bytes / 2;
                int32_t sum = 0;
                for (size_t i = 0; i < numSamples; i++) sum += abs(ptr[i]);
                speakerAmplitude = min(255, (int)(sum / max((size_t)1, numSamples) / 128));

                size_t bw;
                i2s_write(I2S_PORT, spkBuf, bytes, &bw, portMAX_DELAY);
            } else if (speakerDraining) {
                speakerDraining = false;
                speakerActive = false;
                speakerPlaying = false;
                speakerAmplitude = 0;
                if (!micActive) state = AUDIO_IDLE;
                bleSendStatus("PLAY_DONE");
                Serial.println("[AUDIO] Playback complete");
            } else {
                speakerAmplitude = 0;
                feedSilence();
                vTaskDelay(5 / portTICK_PERIOD_MS);
            }
        } else if (!micActive) {
            // Nothing active — feed silence to keep I2S DMA healthy
            feedSilence();
            speakerAmplitude = 0;
            vTaskDelay(20 / portTICK_PERIOD_MS);
        }
    }
}

// ── Public API ──────────────────────────────────────────────

void audioInit() {
    ringBuf = (uint8_t *)heap_caps_malloc(RING_BUF_SIZE, MALLOC_CAP_SPIRAM);
    if (!ringBuf) { Serial.println("[AUDIO] Ring buffer fail!"); return; }

    recBuf = (uint8_t *)heap_caps_malloc(MAX_AUDIO_SIZE, MALLOC_CAP_SPIRAM);
    if (!recBuf) Serial.println("[AUDIO] Recording buffer fail!");
    else Serial.printf("[AUDIO] Recording buffer: %dKB in PSRAM\n", MAX_AUDIO_SIZE / 1024);

    pinMode(PIN_SPEAKER_EN, OUTPUT);
    digitalWrite(PIN_SPEAKER_EN, HIGH);

    if (!initI2S()) { Serial.println("[AUDIO] I2S fail"); return; }
    delay(100);
    initES8311();
    delay(100);
    initES7210();

    xTaskCreatePinnedToCore(audioTask, "audio", 8192, NULL, 5, NULL, 0);
    Serial.println("[AUDIO] Ready");
}

void audioStartMic() {
    micActive = true;
    state = AUDIO_RECORDING;
    Serial.println("[AUDIO] Mic ON");
}

void audioStopMic() {
    micActive = false;
    micAmplitude = 0;
    state = speakerActive ? AUDIO_PLAYING : AUDIO_IDLE;
    Serial.println("[AUDIO] Mic OFF");
}

void audioSpeakerPush(const uint8_t *data, size_t len) {
    if (ringFree() >= len) ringWrite(data, len);
}

void audioStartSpeaker() {
    ringHead = 0; ringTail = 0;
    speakerPlaying = false;
    speakerDraining = false;
    speakerActive = true;
    state = AUDIO_PLAYING;
    Serial.println("[AUDIO] Speaker ON (pre-buffering...)");
}

void audioStopSpeaker() {
    speakerDraining = true;
    // If not playing yet, start now (all data sent)
    if (!speakerPlaying) speakerPlaying = true;
    Serial.printf("[AUDIO] Draining %d bytes\n", ringAvailable());
}

void audioPlayUrl(const char *url) {
    strncpy(playbackUrl, url, sizeof(playbackUrl) - 1);
    wifiPlayRequested = true;
    Serial.printf("[AUDIO] Play URL queued: %s\n", url);
}

void audioStartRecording() {
    recLen = 0;
    recActive = true;
    micAmplitude = 0;
    lastLoudTime = millis();
    audioStartMic();
    Serial.println("[AUDIO] Recording started");
}

void audioStopRecording() {
    recActive = false;
    audioStopMic();
    Serial.printf("[AUDIO] Recording stopped: %d bytes\n", recLen);
}

const uint8_t *audioGetRecording(size_t *len) {
    if (len) *len = recLen;
    return recBuf;
}

int audioGetRecordingLength() { return (int)recLen; }

AudioState audioGetState() { return state; }
bool audioIsMicActive() { return micActive; }
bool audioIsSpeakerActive() { return speakerActive; }
uint8_t audioGetSpeakerAmplitude() { return speakerActive ? speakerAmplitude : micAmplitude; }
