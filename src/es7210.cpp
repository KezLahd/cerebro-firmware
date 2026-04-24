// ES7210 ADC driver — adapted from Waveshare/Espressif example
#ifdef ESP32
#include <Wire.h>
#include <string.h>
#include "esp_log.h"
#include "es7210.h"

#define MCLK_DIV_FRE 256
#define ES7210_MCLK_SOURCE 1 // FROM_CLOCK_DOUBLE_PIN
#define FROM_PAD_PIN 0
#define FROM_CLOCK_DOUBLE_PIN 1
#define I2S_DSP_MODE_A 0

static TwoWire *es7210wire;
static const char *TAG = "ES7210";
static es7210_input_mics_t mic_select = (es7210_input_mics_t)(ES7210_INPUT_MIC1 | ES7210_INPUT_MIC2 | ES7210_INPUT_MIC3 | ES7210_INPUT_MIC4);

struct _coeff_div_es7210 { uint32_t mclk; uint32_t lrck; uint8_t ss_ds, adc_div, dll, doubler, osr, mclk_src; uint32_t lrck_h, lrck_l; };

static const struct _coeff_div_es7210 coeff_div[] = {
    {12288000,8000,0x00,0x03,0x01,0x00,0x20,0x00,0x06,0x00},
    {4096000,8000,0x00,0x01,0x01,0x00,0x20,0x00,0x02,0x00},
    {4096000,16000,0x00,0x01,0x01,0x01,0x20,0x00,0x01,0x00},
    {16384000,16000,0x00,0x02,0x01,0x00,0x20,0x00,0x04,0x00},
    {12288000,16000,0x00,0x03,0x01,0x01,0x20,0x00,0x03,0x00},
    {12288000,48000,0x00,0x01,0x01,0x01,0x20,0x00,0x01,0x00},
    {11289600,44100,0x00,0x01,0x01,0x01,0x20,0x00,0x01,0x00},
};

static esp_err_t es7210_write_reg(uint8_t reg, uint8_t data) {
    es7210wire->beginTransmission(ES7210_ADDR);
    es7210wire->write(reg);
    es7210wire->write(data);
    return es7210wire->endTransmission();
}

static esp_err_t es7210_update_reg_bit(uint8_t reg, uint8_t mask, uint8_t data) {
    uint8_t regv = es7210_read_reg(reg);
    regv = (regv & (~mask)) | (mask & data);
    return es7210_write_reg(reg, regv);
}

int es7210_read_reg(uint8_t reg) {
    es7210wire->beginTransmission(ES7210_ADDR);
    es7210wire->write(reg);
    es7210wire->endTransmission(false);
    es7210wire->requestFrom(ES7210_ADDR, (size_t)1);
    return es7210wire->read();
}

static int get_coeff(uint32_t mclk, uint32_t lrck) {
    for (int i = 0; i < (sizeof(coeff_div)/sizeof(coeff_div[0])); i++)
        if (coeff_div[i].lrck == lrck && coeff_div[i].mclk == mclk) return i;
    return -1;
}

static esp_err_t es7210_config_sample(audio_hal_iface_samples_t sample) {
    int sr = 0;
    switch(sample) {
        case AUDIO_HAL_08K_SAMPLES: sr=8000; break;
        case AUDIO_HAL_16K_SAMPLES: sr=16000; break;
        case AUDIO_HAL_44K_SAMPLES: sr=44100; break;
        case AUDIO_HAL_48K_SAMPLES: sr=48000; break;
        default: sr=16000; break;
    }
    int mclk = sr * MCLK_DIV_FRE;
    int c = get_coeff(mclk, sr);
    if (c < 0) { ESP_LOGE(TAG, "No coeff for %dHz", sr); return ESP_FAIL; }
    uint8_t regv = coeff_div[c].adc_div | (coeff_div[c].doubler<<6) | (coeff_div[c].dll<<7);
    es7210_write_reg(ES7210_MAINCLK_REG02, regv);
    es7210_write_reg(ES7210_OSR_REG07, coeff_div[c].osr);
    es7210_write_reg(ES7210_LRCK_DIVH_REG04, coeff_div[c].lrck_h);
    es7210_write_reg(ES7210_LRCK_DIVL_REG05, coeff_div[c].lrck_l);
    return ESP_OK;
}

esp_err_t es7210_mic_select(es7210_input_mics_t mic) {
    esp_err_t ret = ESP_OK;
    mic_select = mic;
    for (int i = 0; i < 4; i++) ret |= es7210_update_reg_bit(ES7210_MIC1_GAIN_REG43+i, 0x10, 0x00);
    ret |= es7210_write_reg(ES7210_MIC12_POWER_REG4B, 0xff);
    ret |= es7210_write_reg(ES7210_MIC34_POWER_REG4C, 0xff);
    if (mic & ES7210_INPUT_MIC1) { ret|=es7210_update_reg_bit(ES7210_CLOCK_OFF_REG01,0x0b,0x00); ret|=es7210_write_reg(ES7210_MIC12_POWER_REG4B,0x00); ret|=es7210_update_reg_bit(ES7210_MIC1_GAIN_REG43,0x10,0x10); }
    if (mic & ES7210_INPUT_MIC2) { ret|=es7210_update_reg_bit(ES7210_CLOCK_OFF_REG01,0x0b,0x00); ret|=es7210_write_reg(ES7210_MIC12_POWER_REG4B,0x00); ret|=es7210_update_reg_bit(ES7210_MIC2_GAIN_REG44,0x10,0x10); }
    if (mic & ES7210_INPUT_MIC3) { ret|=es7210_update_reg_bit(ES7210_CLOCK_OFF_REG01,0x15,0x00); ret|=es7210_write_reg(ES7210_MIC34_POWER_REG4C,0x00); ret|=es7210_update_reg_bit(ES7210_MIC3_GAIN_REG45,0x10,0x10); }
    if (mic & ES7210_INPUT_MIC4) { ret|=es7210_update_reg_bit(ES7210_CLOCK_OFF_REG01,0x15,0x00); ret|=es7210_write_reg(ES7210_MIC34_POWER_REG4C,0x00); ret|=es7210_update_reg_bit(ES7210_MIC4_GAIN_REG46,0x10,0x10); }
    return ret;
}

esp_err_t es7210_adc_init(TwoWire *tw, audio_hal_codec_config_t *cfg) {
    es7210wire = tw;
    audio_hal_codec_i2s_iface_t *i2s = &cfg->i2s_iface;
    es7210_write_reg(0x00, 0xFF); // Reset
    es7210_write_reg(0x00, 0x41);
    es7210_write_reg(0x01, 0x1F);
    es7210_write_reg(0x09, 0x30);
    es7210_write_reg(0x0A, 0x30);
    if (i2s->mode == AUDIO_HAL_MODE_MASTER) {
        es7210_write_reg(0x08, 0x20);
        es7210_update_reg_bit(0x03, 0x80, ES7210_MCLK_SOURCE ? 0x80 : 0x00);
    }
    // Enable high-pass filter on ADC12 and ADC34 (removes DC offset + low-freq mud)
    es7210_write_reg(ES7210_ADC12_HPF2_REG23, 0x2A);
    es7210_write_reg(ES7210_ADC12_HPF1_REG22, 0x0A);
    es7210_write_reg(ES7210_ADC34_HPF2_REG20, 0x0A);
    es7210_write_reg(ES7210_ADC34_HPF1_REG21, 0x2A);

    es7210_write_reg(0x40, 0xC3);
    es7210_write_reg(0x41, 0x70);
    es7210_write_reg(0x42, 0x70);
    es7210_write_reg(0x07, 0x20);
    es7210_write_reg(0x02, 0xC1);
    es7210_config_sample(i2s->samples);
    es7210_mic_select(mic_select);
    es7210_adc_set_gain_all(GAIN_0DB);
    return ESP_OK;
}

esp_err_t es7210_adc_deinit() { return ESP_OK; }

esp_err_t es7210_adc_config_i2s(audio_hal_codec_mode_t mode, audio_hal_codec_i2s_iface_t *iface) {
    // Set bits
    uint8_t sdp = es7210_read_reg(0x11) & 0x1F;
    if (iface->bits == AUDIO_HAL_BIT_LENGTH_16BITS) sdp |= 0x60;
    else if (iface->bits == AUDIO_HAL_BIT_LENGTH_32BITS) sdp |= 0x80;
    es7210_write_reg(0x11, sdp);
    // Set format (I2S normal)
    sdp = es7210_read_reg(0x11) & 0xFC;
    es7210_write_reg(0x11, sdp);
    es7210_write_reg(0x12, 0x00);
    return ESP_OK;
}

static esp_err_t es7210_start(uint8_t clk) {
    es7210_write_reg(0x01, clk);
    es7210_write_reg(0x06, 0x00);
    es7210_write_reg(0x47, 0x00);
    es7210_write_reg(0x48, 0x00);
    es7210_write_reg(0x49, 0x00);
    es7210_write_reg(0x4A, 0x00);
    es7210_mic_select(mic_select);
    return ESP_OK;
}

esp_err_t es7210_adc_ctrl_state(audio_hal_codec_mode_t mode, audio_hal_ctrl_t ctrl) {
    static uint8_t regv;
    int ret = es7210_read_reg(0x01);
    if (ret != 0x7F && ret != 0xFF) regv = ret;
    if (ctrl == AUDIO_HAL_CTRL_START) es7210_start(regv);
    return ESP_OK;
}

esp_err_t es7210_adc_set_gain(es7210_input_mics_t mic, es7210_gain_value_t g) {
    if (g > GAIN_37_5DB) g = GAIN_37_5DB;
    if (mic & ES7210_INPUT_MIC1) es7210_update_reg_bit(0x43, 0x0F, g);
    if (mic & ES7210_INPUT_MIC2) es7210_update_reg_bit(0x44, 0x0F, g);
    if (mic & ES7210_INPUT_MIC3) es7210_update_reg_bit(0x45, 0x0F, g);
    if (mic & ES7210_INPUT_MIC4) es7210_update_reg_bit(0x46, 0x0F, g);
    return ESP_OK;
}

esp_err_t es7210_adc_set_gain_all(es7210_gain_value_t g) {
    return es7210_adc_set_gain(mic_select, g);
}
#endif
