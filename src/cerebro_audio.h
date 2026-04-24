#pragma once
#include <Arduino.h>

enum AudioState {
    AUDIO_IDLE,
    AUDIO_RECORDING,   // Mic recording to buffer
    AUDIO_PLAYING       // Playing audio through speaker
};

// Init I2S + codecs + audio task
void audioInit();

// Mic control (raw — used by BLE pairing test only)
void audioStartMic();
void audioStopMic();

// Recording: mic → PSRAM buffer, download via HTTP
void audioStartRecording();
void audioStopRecording();
const uint8_t *audioGetRecording(size_t *len);
int audioGetRecordingLength();

// Speaker: push a PCM chunk into playback ring buffer
void audioSpeakerPush(const uint8_t *data, size_t len);
void audioStartSpeaker();
void audioStopSpeaker();

// WiFi playback — fetch audio from URL and play
void audioPlayUrl(const char *url);

// State
AudioState audioGetState();
bool audioIsMicActive();
bool audioIsSpeakerActive();

// Get current speaker audio amplitude (0-255) for mouth animation
uint8_t audioGetSpeakerAmplitude();
