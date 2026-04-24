// ═══════════════════════════════════════════════════════════
//  CEREBRO — Animated Face Display with Tracking Eyes
//  Waveshare ESP32-S3-Touch-AMOLED-1.75C (466x466 CO5300)
// ═══════════════════════════════════════════════════════════

#include <Arduino.h>
#include <Wire.h>
#include <Arduino_GFX_Library.h>

#define XPOWERS_CHIP_AXP2101
#include <XPowersLib.h>

#include "cerebro_ble.h"
#include "cerebro_audio.h"
#include "cerebro_wifi.h"

// Touch via raw I2C (CST9217)

// ═══════════════════════════════════════════════════════════
//  DISPLAY HARDWARE
// ═══════════════════════════════════════════════════════════

#define PIN_LCD_CS   12
#define PIN_LCD_SCLK 38
#define PIN_LCD_D0   4
#define PIN_LCD_D1   5
#define PIN_LCD_D2   6
#define PIN_LCD_D3   7
#define PIN_LCD_RST  2

static Arduino_DataBus *bus = new Arduino_ESP32QSPI(
    PIN_LCD_CS, PIN_LCD_SCLK, PIN_LCD_D0, PIN_LCD_D1, PIN_LCD_D2, PIN_LCD_D3
);
static Arduino_CO5300 *display = new Arduino_CO5300(bus, PIN_LCD_RST, 0, false, 466, 466);
static Arduino_Canvas *canvas = new Arduino_Canvas(466, 466, display);

// ═══════════════════════════════════════════════════════════
//  BUTTONS
// ═══════════════════════════════════════════════════════════

#define PIN_BOOT 0  // GPIO0, active LOW

// ═══════════════════════════════════════════════════════════
//  IMU (QMI8658)
// ═══════════════════════════════════════════════════════════

#define QMI8658_ADDR 0x6B

static uint8_t imu_read(uint8_t reg) {
    Wire.beginTransmission(QMI8658_ADDR);
    Wire.write(reg);
    Wire.endTransmission(false);
    Wire.requestFrom((uint8_t)QMI8658_ADDR, (uint8_t)1);
    return Wire.available() ? Wire.read() : 0;
}

static void imu_write(uint8_t reg, uint8_t val) {
    Wire.beginTransmission(QMI8658_ADDR);
    Wire.write(reg);
    Wire.write(val);
    Wire.endTransmission();
}

static int16_t imu_read16(uint8_t regL) {
    uint8_t lo = imu_read(regL);
    uint8_t hi = imu_read(regL + 1);
    return (int16_t)((hi << 8) | lo);
}

static bool imuAvailable = false;
static bool rotationLocked = false;
static float currentAngle = 0;
static float smoothAngle = 0;

bool imuInit() {
    if (imu_read(0x00) != 0x05) {
        Serial.println("[IMU] QMI8658 not found");
        return false;
    }
    imu_write(0x60, 0xB0);
    delay(50);
    imu_write(0x02, 0x60);
    imu_write(0x03, 0x23);
    imu_write(0x04, 0x53);
    imu_write(0x06, 0x00);
    imu_write(0x08, 0x03);
    delay(50);
    Serial.println("[IMU] QMI8658 OK");
    return true;
}

void imuUpdateAngle() {
    if (!imuAvailable || rotationLocked) return;

    float ax = imu_read16(0x35) / 4096.0f;
    float ay = imu_read16(0x37) / 4096.0f;

    // Skip if device is flat (no meaningful tilt)
    if (fabsf(ax) < 0.05f && fabsf(ay) < 0.05f) return;

    // Calculate tilt angle (which way is "down")
    // atan2 gives angle of gravity vector, offset to match PCB mounting
    float target = atan2f(-ay, ax);

    // Normalize to -PI..PI
    while (target > PI) target -= TWO_PI;
    while (target < -PI) target += TWO_PI;

    // Smooth toward target (handle wraparound at ±PI)
    float diff = target - smoothAngle;
    if (diff > PI) diff -= TWO_PI;
    if (diff < -PI) diff += TWO_PI;
    smoothAngle += diff * 0.7f;  // near-instant tracking

    // Normalize
    while (smoothAngle > PI) smoothAngle -= TWO_PI;
    while (smoothAngle < -PI) smoothAngle += TWO_PI;

    currentAngle = smoothAngle;
}

// ═══════════════════════════════════════════════════════════
//  COLOURS
// ═══════════════════════════════════════════════════════════

#define COL_BLACK     0x0000
#define COL_WHITE     0xFFFF
#define COL_RED       0xC0E7  // #C41E3A
#define COL_DARK_GREY 0x3186
#define COL_PUPIL     0x1082  // very dark grey (not pure black, slight depth)
#define COL_GREEN     0x4EA4  // #4CAF50

// ═══════════════════════════════════════════════════════════
//  BATTERY (AXP2101)
// ═══════════════════════════════════════════════════════════

static XPowersPMU pmu;
static bool pmuAvailable = false;
static int battPercent = -1;
static float smoothPercent = -1;
static bool battCharging = false;
static unsigned long lastBattRead = 0;
#define BATT_READ_INTERVAL 5000  // read every 5s

bool battInit() {
    bool ok = pmu.begin(Wire, AXP2101_SLAVE_ADDRESS, 15, 14);
    if (!ok) {
        Serial.println("[BATT] AXP2101 not found");
        return false;
    }
    pmu.enableBattVoltageMeasure();
    pmu.enableBattDetection();
    Serial.println("[BATT] AXP2101 OK");
    return true;
}

void battUpdate() {
    if (!pmuAvailable) return;
    if (millis() - lastBattRead < BATT_READ_INTERVAL) return;
    lastBattRead = millis();

    battCharging = pmu.isCharging();

    // Read voltage (in mV)
    float voltage = pmu.getBattVoltage();

    // No battery if voltage too low
    if (voltage < 500) {
        battPercent = -1;
        smoothPercent = -1;
        return;
    }

    // Voltage-based percentage (3.2V=0%, 4.15V=100%)
    // Use slightly narrower range to avoid false 0%/100%
    float pct = (voltage - 3200.0f) / (4150.0f - 3200.0f) * 100.0f;

    // When charging, voltage is inflated — compensate roughly
    if (battCharging) pct -= 5.0f;

    int rawPercent = constrain((int)pct, 0, 100);

    // Smooth: move max 2% per reading to avoid jumps
    if (smoothPercent < 0) {
        smoothPercent = rawPercent;
    } else {
        float diff = rawPercent - smoothPercent;
        if (diff > 2) diff = 2;
        if (diff < -2) diff = -2;
        smoothPercent += diff;
    }
    battPercent = constrain((int)smoothPercent, 0, 100);
}

void drawBatteryIcon(int cx, int cy) {
    // No battery connected
    if (battPercent < 0) {
        canvas->setTextSize(2);
        canvas->setTextColor(COL_DARK_GREY);
        const char *msg = "NO BATT";
        int tw = strlen(msg) * 12;
        canvas->setCursor(cx - tw/2, cy - 6);
        canvas->print(msg);
        return;
    }

    // Battery body: 40x18 rounded rect, nub on right
    int bw = 40, bh = 18;
    int bx = cx - bw/2, by = cy - bh/2;

    // Thick outline (2px)
    canvas->drawRoundRect(bx, by, bw, bh, 3, COL_WHITE);
    canvas->drawRoundRect(bx+1, by+1, bw-2, bh-2, 2, COL_WHITE);
    // Terminal nub
    canvas->fillRoundRect(bx + bw, cy - 4, 4, 8, 1, COL_WHITE);

    // Fill level inside (3px padding)
    int fillMaxW = bw - 6;
    int fillW = (battPercent * fillMaxW) / 100;
    uint16_t fillCol = COL_GREEN;
    if (battPercent <= 20) fillCol = COL_RED;
    else if (battPercent <= 50) fillCol = COL_WHITE;

    if (fillW > 0) {
        canvas->fillRect(bx + 3, by + 3, fillW, bh - 6, fillCol);
    }

    // Charging: draw a solid bolt shape inside the battery
    if (battCharging) {
        int lx = cx, ly = cy;
        // Thick bolt using filled triangles
        canvas->fillTriangle(lx+1, ly-6, lx-5, ly+1, lx+1, ly+1, COL_BLACK);
        canvas->fillTriangle(lx-1, ly-1, lx+5, ly-1, lx-1, ly+6, COL_BLACK);
        // Slightly inset white bolt for contrast
        canvas->fillTriangle(lx+1, ly-5, lx-4, ly+1, lx+1, ly+1, COL_WHITE);
        canvas->fillTriangle(lx-1, ly-1, lx+4, ly-1, lx-1, ly+5, COL_WHITE);
    }

    // Percentage text below — size 2 for readability
    canvas->setTextSize(2);
    canvas->setTextColor(COL_WHITE);
    char buf[8];
    snprintf(buf, sizeof(buf), "%d%%", battPercent);
    int tw = strlen(buf) * 12;
    canvas->setCursor(cx - tw/2, cy + bh/2 + 4);
    canvas->print(buf);
}

// ═══════════════════════════════════════════════════════════
//  FACE LAYOUT
// ═══════════════════════════════════════════════════════════

const int FACE_CX = 233;
const int FACE_CY = 233;

// Eye sockets
const int EYE_SPACING = 55;
const int EYE_Y = -20;
const int SCLERA_RX = 20;      // white eye socket horizontal radius (narrow)
const int SCLERA_RY = 38;      // vertical radius (tall pill shape)

// Pupils
const int PUPIL_R = 16;        // dark pupil radius (bigger)
const int MAX_PUPIL_OFFSET = 22; // max — pupils go right to sclera edge
const int HIGHLIGHT_R = 5;     // specular highlight dot radius
const int HIGHLIGHT_OX = 5;    // highlight offset from pupil center
const int HIGHLIGHT_OY = -5;

// Touch (CST9217)
#define PIN_TOUCH_SDA 15
#define PIN_TOUCH_SCL 14
#define PIN_TOUCH_INT 11
#define CST9217_ADDR  0x5A

// Brows
const int BROW_ABOVE_EYE = 35;
const int BROW_HALF_LEN = 16;
const int BROW_THICKNESS = 7;
const int BROW_CURVE = 6;

// Mouth
const int MOUTH_Y = 50;
const int MOUTH_THICKNESS = 4;

// Animation
const float MORPH_SPEED = 0.08f;
const float PUPIL_LERP = 0.35f;  // how fast pupils track (higher = snappier)
const float LOOK_DURATION = 300; // ms to hold look after last touch/button

// ═══════════════════════════════════════════════════════════
//  EXPRESSION SYSTEM
// ═══════════════════════════════════════════════════════════

enum MouthShape { MOUTH_O, MOUTH_SMILE, MOUTH_FROWN, MOUTH_LINE, MOUTH_SMALL_O };
enum EyeShape   { EYE_OPEN, EYE_CLOSED_U };

struct BrowDef { int innerDy, outerDy, raise; };

struct ExpressionDef {
    BrowDef leftBrow, rightBrow;
    int browSpread, browCurve, browLen, browThick, browGap;
    EyeShape eyeShape;
    int eyeRX, eyeRY;      // sclera radii for this expression
    int pupilR;             // pupil size (0 = use default PUPIL_R)
    MouthShape mouth;
    int mouthW, mouthH;
};

enum ExpressionType {
    EXPR_NEUTRAL, EXPR_SURPRISED, EXPR_ANGRY, EXPR_HAPPY,
    EXPR_SAD, EXPR_THINKING, EXPR_PENSIVE, EXPR_COUNT
};

//                        lBrow               rBrow               sprd crv len thk gap  eyes        sRX sRY  pR mouth        mW  mH
const ExpressionDef EXPRESSIONS[EXPR_COUNT] = {
    /* NEUTRAL   */ {{ 3,-3, 0},  { 3,-3, 0},   0,  6,16, 7,42, EYE_OPEN,    20, 38, 16, MOUTH_O,      22, 26},
    /* SURPRISED */ {{ 8,-8,-10}, { 8,-8,-10},   0,  6,16, 7,46, EYE_OPEN,    22, 44, 14, MOUTH_O,      30, 38},
    /* ANGRY     */ {{-8, 6, 6},  {-8, 6, 6},   15,  6,16, 7,38, EYE_OPEN,    20, 28, 18, MOUTH_LINE,   32,  4},
    /* HAPPY     */ {{ 3,-3,-3},  { 3,-3,-3},    0,  6,16, 7,42, EYE_OPEN,    20, 34, 16, MOUTH_SMILE,  32, 18},
    /* SAD       */ {{-4, 4, 3},  {-4, 4, 3},   15,  6,16, 7,42, EYE_OPEN,    18, 36, 16, MOUTH_FROWN,  26, 14},
    /* THINKING  */ {{ 8,-8,-8},  { 0, 0, 0},    0,  6,16, 7,42, EYE_OPEN,    20, 38, 16, MOUTH_SMALL_O,16, 18},
    /* PENSIVE   */ {{-5, 5, 3},  {-5, 5, 3},   10, -6,12, 4,48, EYE_CLOSED_U,18, 14,  0, MOUTH_LINE,   18,  4},
};

// ═══════════════════════════════════════════════════════════
//  STATE
// ═══════════════════════════════════════════════════════════

static ExpressionType targetExpr = EXPR_SURPRISED;

// Morph state
static struct {
    float lbInnerDy, lbOuterDy, lbRaise;
    float rbInnerDy, rbOuterDy, rbRaise;
    float browSpread, browCurve, browLen, browThick, browGap;
    float eyeRX, eyeRY, pupilR;
    float mouthW, mouthH;
    MouthShape mouth;
    EyeShape eyeShape;
} morph;

// Pupil tracking — current and target positions (offsets from eye center)
static float pupilCurX = 0, pupilCurY = 0;   // current (lerped)
static float pupilTgtX = 0, pupilTgtY = 0;   // target

// Idle saccade
static unsigned long nextSaccadeTime = 0;

// Button look
static unsigned long lookUntil = 0;  // millis when button-look expires

// Blink
static unsigned long lastBlinkTime = 0;
static unsigned long nextBlinkInterval = 4000;
static bool isBlinking = false;
static unsigned long blinkStartTime = 0;
static const unsigned long BLINK_DURATION = 200;

// Frame timing
static unsigned long lastFrameTime = 0;
static const unsigned long FRAME_MS = 33;

// ═══════════════════════════════════════════════════════════
//  CONTROL PANEL (swipe up from bottom)
// ═══════════════════════════════════════════════════════════

// Forward declarations for panel code
static float lerpf(float a, float b, float t);
void drawThickArc(int cx, int cy, int rx, int ry, float startAngle, float endAngle, int thickness, uint16_t color);
void drawThickLine(int x0, int y0, int x1, int y1, int thickness, uint16_t color);

static bool panelVisible = false;
static float panelSlide = 0;        // 0 = hidden, 1 = fully visible
static float panelSlideTarget = 0;

// Swipe tracking
static int swipeStartY = -1;
static bool swipeActive = false;
static unsigned long panelShowTime = 0;

// Dark plum brand colour for panel bg
#define COL_PLUM 0x3082  // dark burgundy ~(48, 12, 16)

void drawControlPanel() {
    float s = panelSlide;
    if (s < 0.01f) return;

    // Panel slides up from bottom — offset everything by (1-s)*466
    int slideOffset = (int)((1.0f - s) * 466);

    // Full-screen plum background (slides up)
    canvas->fillRect(0, slideOffset, 466, 466, COL_PLUM);

    // Grab handle bar
    canvas->fillRoundRect(233 - 30, slideOffset + 60, 60, 5, 2, COL_DARK_GREY);

    int btnCX = 233;
    int btnCY = slideOffset + 210;
    int btnR = 45;

    if (rotationLocked) {
        canvas->fillCircle(btnCX, btnCY, btnR, COL_WHITE);
        uint16_t iconCol = COL_PLUM;
        canvas->fillRoundRect(btnCX - 14, btnCY - 2, 28, 22, 4, iconCol);
        drawThickArc(btnCX, btnCY - 8, 9, 12, PI + 0.2f, TWO_PI - 0.2f, 4, iconCol);
    } else {
        for (int i = 0; i < 3; i++) canvas->drawCircle(btnCX, btnCY, btnR - i, COL_WHITE);
        canvas->fillRoundRect(btnCX - 14, btnCY - 2, 28, 22, 4, COL_WHITE);
        drawThickArc(btnCX + 5, btnCY - 8, 9, 12, PI + 0.2f, TWO_PI - 0.2f, 3, COL_WHITE);
    }

    // Label
    canvas->setTextSize(2);
    canvas->setTextColor(COL_WHITE);
    const char *label = rotationLocked ? "Rotation Locked" : "Auto Rotate";
    int labelW = strlen(label) * 12;
    canvas->setCursor(btnCX - labelW / 2, btnCY + btnR + 25);
    canvas->print(label);

    // Hint
    canvas->setTextSize(1);
    canvas->setTextColor(COL_DARK_GREY);
    const char *hint = "Tap to toggle. Swipe down to close.";
    int hintW = strlen(hint) * 6;
    canvas->setCursor(233 - hintW / 2, slideOffset + 360);
    canvas->print(hint);
}

// Rotate raw touch coords into face-space (accounts for gyro rotation)
void touchToFaceSpace(int rawX, int rawY, int &faceX, int &faceY) {
    float fx = rawX - 233, fy = rawY - 233;
    float ca = cosf(-currentAngle), sa = sinf(-currentAngle);
    faceX = (int)(fx * ca - fy * sa) + 233;
    faceY = (int)(fx * sa + fy * ca) + 233;
}

void handleSwipe(int rawX, int rawY, bool touching) {
    // Convert to face-space so "bottom" is always relative to the face
    int tx, ty;
    touchToFaceSpace(rawX, rawY, tx, ty);

    if (touching) {
        if (swipeStartY < 0) {
            swipeStartY = ty;
            swipeActive = false;
        }

        // Swipe up from bottom (in face-space)
        if (!panelVisible && swipeStartY > 380 && (swipeStartY - ty) > 60) {
            panelVisible = true;
            panelSlideTarget = 1.0f;
            panelShowTime = millis();
            swipeActive = true;
        }

        // Swipe down to dismiss
        if (panelVisible && swipeStartY < 300 && (ty - swipeStartY) > 60) {
            panelVisible = false;
            panelSlideTarget = 0;
            swipeActive = true;
        }

        // Tap on lock button
        if (panelVisible && panelSlide > 0.8f && !swipeActive) {
            int dx = tx - 233, dy = ty - 200;
            if (dx * dx + dy * dy < 50 * 50) {
                rotationLocked = !rotationLocked;
                if (rotationLocked) {
                    currentAngle = 0;
                    smoothAngle = 0;
                }
                Serial.printf("[PANEL] Rotation %s\n", rotationLocked ? "LOCKED" : "AUTO");
                swipeActive = true;
            }
        }
    } else {
        swipeStartY = -1;
        swipeActive = false;
    }

    panelSlide = lerpf(panelSlide, panelSlideTarget, 0.25f);

    // No auto-dismiss — user must swipe down to close
}

// Rotation buffer (PSRAM) for pixel-level rotation
static uint16_t *rotBuf = nullptr;

void flushWithRotation() {
    if (fabsf(currentAngle) < 0.02f) {
        canvas->flush();
        return;
    }

    uint16_t *fb = canvas->getFramebuffer();
    const int N = 466;
    const int C = 233;

    // Fixed-point trig (10-bit fraction)
    int cosA = (int)(cosf(-currentAngle) * 1024);
    int sinA = (int)(sinf(-currentAngle) * 1024);

    for (int y = 0; y < N; y++) {
        int fy = y - C;
        int rowCos = fy * cosA;
        int rowSin = fy * sinA;

        for (int x = 0; x < N; x++) {
            int fx = x - C;
            int sx = ((fx * cosA - rowSin) >> 10) + C;
            int sy = ((fx * sinA + rowCos) >> 10) + C;

            if (sx >= 0 && sx < N && sy >= 0 && sy < N)
                rotBuf[y * N + x] = fb[sy * N + sx];
            else
                rotBuf[y * N + x] = 0;
        }
    }

    memcpy(fb, rotBuf, N * N * sizeof(uint16_t));
    canvas->flush();
}

// ═══════════════════════════════════════════════════════════
//  MORPH INIT / UPDATE
// ═══════════════════════════════════════════════════════════

static float lerpf(float a, float b, float t) { return a + (b - a) * t; }

static void morphInit(ExpressionType expr) {
    const ExpressionDef &e = EXPRESSIONS[expr];
    morph.lbInnerDy = e.leftBrow.innerDy;  morph.lbOuterDy = e.leftBrow.outerDy;  morph.lbRaise = e.leftBrow.raise;
    morph.rbInnerDy = e.rightBrow.innerDy; morph.rbOuterDy = e.rightBrow.outerDy; morph.rbRaise = e.rightBrow.raise;
    morph.browSpread = e.browSpread; morph.browCurve = e.browCurve; morph.browLen = e.browLen;
    morph.browThick = e.browThick;   morph.browGap = e.browGap;
    morph.eyeRX = e.eyeRX; morph.eyeRY = e.eyeRY; morph.pupilR = e.pupilR > 0 ? e.pupilR : PUPIL_R;
    morph.mouthW = e.mouthW; morph.mouthH = e.mouthH;
    morph.mouth = e.mouth; morph.eyeShape = e.eyeShape;
}

static void morphUpdate() {
    const ExpressionDef &e = EXPRESSIONS[targetExpr];
    float t = MORPH_SPEED;
    morph.lbInnerDy = lerpf(morph.lbInnerDy, e.leftBrow.innerDy, t);
    morph.lbOuterDy = lerpf(morph.lbOuterDy, e.leftBrow.outerDy, t);
    morph.lbRaise   = lerpf(morph.lbRaise,   e.leftBrow.raise,   t);
    morph.rbInnerDy = lerpf(morph.rbInnerDy, e.rightBrow.innerDy, t);
    morph.rbOuterDy = lerpf(morph.rbOuterDy, e.rightBrow.outerDy, t);
    morph.rbRaise   = lerpf(morph.rbRaise,   e.rightBrow.raise,   t);
    morph.browSpread = lerpf(morph.browSpread, e.browSpread, t);
    morph.browCurve  = lerpf(morph.browCurve,  e.browCurve,  t);
    morph.browLen    = lerpf(morph.browLen,    e.browLen,     t);
    morph.browThick  = lerpf(morph.browThick,  e.browThick,  t);
    morph.browGap    = lerpf(morph.browGap,    e.browGap,    t);
    morph.eyeRX = lerpf(morph.eyeRX, e.eyeRX, t);
    morph.eyeRY = lerpf(morph.eyeRY, e.eyeRY, t);
    morph.pupilR = lerpf(morph.pupilR, e.pupilR > 0 ? e.pupilR : PUPIL_R, t);
    morph.mouthW = lerpf(morph.mouthW, e.mouthW, t);
    morph.mouthH = lerpf(morph.mouthH, e.mouthH, t);
    morph.mouth = e.mouth;
    morph.eyeShape = e.eyeShape;
}

// ═══════════════════════════════════════════════════════════
//  PUPIL TRACKING
// ═══════════════════════════════════════════════════════════

void setPupilTarget(float tx, float ty) {
    // Clamp to max offset circle
    float dist = sqrtf(tx * tx + ty * ty);
    if (dist > MAX_PUPIL_OFFSET) {
        float scale = MAX_PUPIL_OFFSET / dist;
        tx *= scale;
        ty *= scale;
    }
    pupilTgtX = tx;
    pupilTgtY = ty;
}

void lookAtScreen(int screenX, int screenY) {
    float dx = screenX - FACE_CX;
    float dy = screenY - (FACE_CY + EYE_Y);
    float dist = sqrtf(dx * dx + dy * dy);
    if (dist < 5.0f) return;

    float tx = (dx / dist) * MAX_PUPIL_OFFSET;
    float ty = (dy / dist) * MAX_PUPIL_OFFSET;
    setPupilTarget(tx, ty);
    lookUntil = millis() + (unsigned long)LOOK_DURATION;
    nextSaccadeTime = lookUntil + 500; // don't saccade right after touch
}

void lookAt(float dirX, float dirY) {
    setPupilTarget(dirX * MAX_PUPIL_OFFSET, dirY * MAX_PUPIL_OFFSET);
    lookUntil = millis() + (unsigned long)LOOK_DURATION;
    nextSaccadeTime = lookUntil + 500;
}

void updatePupils() {
    unsigned long now = millis();

    // If a button-look is active, hold that target
    if (now < lookUntil) {
        // target already set by lookAt()
    } else {
        // Idle saccade — small random glances
        if (now > nextSaccadeTime) {
            float angle = random(0, 628) / 100.0f;
            float radius = random(3, MAX_PUPIL_OFFSET);
            setPupilTarget(cosf(angle) * radius, sinf(angle) * radius);
            nextSaccadeTime = now + random(800, 3000);
        }
    }

    // Smooth lerp toward target
    pupilCurX = lerpf(pupilCurX, pupilTgtX, PUPIL_LERP);
    pupilCurY = lerpf(pupilCurY, pupilTgtY, PUPIL_LERP);
}

// ═══════════════════════════════════════════════════════════
//  BLINK
// ═══════════════════════════════════════════════════════════

float getBlinkFactor() {
    if (!isBlinking) return 1.0f;
    unsigned long elapsed = millis() - blinkStartTime;
    if (elapsed >= BLINK_DURATION) { isBlinking = false; return 1.0f; }
    float t = (float)elapsed / BLINK_DURATION;
    if (t < 0.35f) return 1.0f - (t / 0.35f);
    if (t < 0.65f) return 0.0f;
    return (t - 0.65f) / 0.35f;
}

void updateBlink() {
    if (!isBlinking && millis() - lastBlinkTime > nextBlinkInterval) {
        isBlinking = true;
        blinkStartTime = millis();
        lastBlinkTime = millis();
        nextBlinkInterval = 3000 + random(2000);
    }
}

// ═══════════════════════════════════════════════════════════
//  DRAWING HELPERS
// ═══════════════════════════════════════════════════════════

void drawThickLine(int x0, int y0, int x1, int y1, int thickness, uint16_t color) {
    float dx = x1 - x0, dy = y1 - y0;
    float len = sqrtf(dx * dx + dy * dy);
    if (len < 0.5f) return;
    float nx = -dy / len * (thickness / 2.0f);
    float ny =  dx / len * (thickness / 2.0f);
    canvas->fillTriangle(x0-nx,y0-ny, x0+nx,y0+ny, x1+nx,y1+ny, color);
    canvas->fillTriangle(x0-nx,y0-ny, x1-nx,y1-ny, x1+nx,y1+ny, color);
    canvas->fillCircle(x0, y0, thickness/2, color);
    canvas->fillCircle(x1, y1, thickness/2, color);
}

void drawThickArc(int cx, int cy, int rx, int ry,
                  float startAngle, float endAngle,
                  int thickness, uint16_t color) {
    int dotR = thickness / 2;
    for (float a = startAngle; a <= endAngle; a += 0.04f) {
        canvas->fillCircle(cx + (int)(cosf(a)*rx), cy + (int)(sinf(a)*ry), dotR, color);
    }
}

// ═══════════════════════════════════════════════════════════
//  FACE ELEMENT DRAWERS
// ═══════════════════════════════════════════════════════════

void drawEyeWithPupil(int cx, int cy, int scleraRX, int scleraRY,
                      int pupilR, float pupilOX, float pupilOY,
                      float blink) {
    // Blink squishes the sclera vertically
    int sRY = max(3, (int)(scleraRY * blink));

    // 1. White sclera — pill/capsule shape (rounded rect with radius = half-width)
    int rectX = cx - scleraRX;
    int rectY = cy - sRY;
    int rectW = scleraRX * 2;
    int rectH = sRY * 2;
    int cornerR = scleraRX;  // fully rounded top & bottom, straight sides
    canvas->fillRoundRect(rectX, rectY, rectW, rectH, cornerR, COL_WHITE);

    // Only draw pupil + highlight if eye is open enough
    if (blink > 0.3f) {
        int px = cx + (int)pupilOX;
        int py = cy + (int)pupilOY;

        // Let pupil move freely — clamp only to keep it overlapping the sclera
        float maxOY = max(0, sRY - pupilR);
        py = constrain(py, cy - (int)maxOY, cy + (int)maxOY);
        // X: allow pupil center up to sclera edge (pupil will overflow into black bg)
        px = constrain(px, cx - scleraRX, cx + scleraRX);

        // 2. Dark pupil (drawn after sclera, overflow into black bg is invisible)
        canvas->fillCircle(px, py, pupilR, COL_PUPIL);

        // 3. Specular highlight
        canvas->fillCircle(px + HIGHLIGHT_OX, py + HIGHLIGHT_OY, HIGHLIGHT_R, COL_WHITE);
    }
}

void drawEyeClosedU(int cx, int cy, int rx, int ry, int thickness, uint16_t color) {
    drawThickArc(cx, cy - ry/2, rx, ry, 0.15f, PI - 0.15f, thickness, color);
}

void drawBrow(int cx, int cy, const BrowDef &def, int halfLen, int thickness,
              int curve, bool isLeft, uint16_t color) {
    int innerX = isLeft ? cx + halfLen : cx - halfLen;
    int outerX = isLeft ? cx - halfLen : cx + halfLen;
    int browBaseY = cy + def.raise;
    int innerY = browBaseY + def.innerDy;
    int outerY = browBaseY + def.outerDy;
    int dotR = thickness / 2;
    int midX = (innerX + outerX) / 2;
    int midY = (innerY + outerY) / 2 - curve;
    for (float t = 0; t <= 1.0f; t += 0.04f) {
        float inv = 1.0f - t;
        float px = inv*inv*outerX + 2*inv*t*midX + t*t*innerX;
        float py = inv*inv*outerY + 2*inv*t*midY + t*t*innerY;
        canvas->fillCircle((int)px, (int)py, dotR, color);
    }
}

void drawMouth(int cx, int cy, MouthShape shape, int w, int h,
               int thickness, uint16_t color) {
    switch (shape) {
        case MOUTH_O:
        case MOUTH_SMALL_O:
            drawThickArc(cx, cy, w, h, 0, TWO_PI, thickness, color);
            break;
        case MOUTH_SMILE:
            drawThickArc(cx, cy, w, h, 0.1f, PI - 0.1f, thickness, color);
            break;
        case MOUTH_FROWN:
            drawThickArc(cx, cy, w, h, PI + 0.1f, TWO_PI - 0.1f, thickness, color);
            break;
        case MOUTH_LINE:
            drawThickLine(cx - w, cy, cx + w, cy, thickness, color);
            break;
    }
}

// ═══════════════════════════════════════════════════════════
//  FACE COMPOSITOR
// ═══════════════════════════════════════════════════════════

void drawFace() {
    morphUpdate();
    updatePupils();
    imuUpdateAngle();
    float blink = getBlinkFactor();

    int leftEyeX  = FACE_CX - EYE_SPACING;
    int rightEyeX = FACE_CX + EYE_SPACING;
    int eyeY = FACE_CY + EYE_Y;

    int browY = eyeY - (int)morph.browGap;
    int mouthY = FACE_CY + MOUTH_Y;

    BrowDef lbDef = {(int)morph.lbInnerDy, (int)morph.lbOuterDy, (int)morph.lbRaise};
    BrowDef rbDef = {(int)morph.rbInnerDy, (int)morph.rbOuterDy, (int)morph.rbRaise};

    int spread = (int)morph.browSpread;
    int curve  = (int)morph.browCurve;
    int bLen   = (int)morph.browLen;
    int bThick = max(2, (int)morph.browThick);
    int sRX    = (int)morph.eyeRX;
    int sRY    = (int)morph.eyeRY;
    int pR     = max(4, (int)morph.pupilR);

    canvas->fillScreen(COL_BLACK);

    // 1. Brows
    drawBrow(leftEyeX - spread,  browY, lbDef,  bLen, bThick, curve, true,  COL_WHITE);
    drawBrow(rightEyeX + spread, browY, rbDef,  bLen, bThick, curve, false, COL_WHITE);

    // 2. Eyes
    if (morph.eyeShape == EYE_CLOSED_U) {
        drawEyeClosedU(leftEyeX,  eyeY, sRX, sRY, bThick, COL_WHITE);
        drawEyeClosedU(rightEyeX, eyeY, sRX, sRY, bThick, COL_WHITE);
    } else {
        drawEyeWithPupil(leftEyeX,  eyeY, sRX, sRY, pR, pupilCurX, pupilCurY, blink);
        drawEyeWithPupil(rightEyeX, eyeY, sRX, sRY, pR, pupilCurX, pupilCurY, blink);
    }

    // 3. Mouth
    drawMouth(FACE_CX, mouthY, morph.mouth,
              (int)morph.mouthW, (int)morph.mouthH,
              MOUTH_THICKNESS, COL_WHITE);

    // Battery icon at top center
    battUpdate();
    drawBatteryIcon(FACE_CX, 35);

    // Control panel overlay (drawn on top of face, before rotation)
    drawControlPanel();

    flushWithRotation();
}

// ═══════════════════════════════════════════════════════════
//  EXPRESSION API
// ═══════════════════════════════════════════════════════════

void setExpression(ExpressionType expr) {
    if (expr >= EXPR_COUNT) return;
    targetExpr = expr;
    Serial.printf("[FACE] Expression → %d\n", expr);
}

// ═══════════════════════════════════════════════════════════
//  BUTTON HANDLING
// ═══════════════════════════════════════════════════════════

// ═══════════════════════════════════════════════════════════
//  TOUCH (CST9217 via SensorLib)
// ═══════════════════════════════════════════════════════════

#define PIN_TOUCH_RST 2  // shared with LCD_RST on 1.75C

static bool touchAvailable = false;

bool touchInit() {
    // Reset touch controller (Waveshare official sequence)
    pinMode(PIN_TOUCH_RST, OUTPUT);
    digitalWrite(PIN_TOUCH_RST, LOW);
    delay(30);
    digitalWrite(PIN_TOUCH_RST, HIGH);
    delay(50);
    delay(1000);

    Wire.begin(PIN_TOUCH_SDA, PIN_TOUCH_SCL);

    Wire.beginTransmission(CST9217_ADDR);
    if (Wire.endTransmission() != 0) {
        Serial.println("[TOUCH] CST9217 not found");
        return false;
    }

    pinMode(PIN_TOUCH_INT, INPUT_PULLUP);
    Serial.println("[TOUCH] CST9217 OK");
    return true;
}

bool readTouch(int &x, int &y) {
    if (!touchAvailable) return false;

    // CST9217 uses 16-bit register addresses: read from 0xD000
    Wire.beginTransmission(CST9217_ADDR);
    Wire.write((uint8_t)0xD0);
    Wire.write((uint8_t)0x00);
    if (Wire.endTransmission() != 0) return false;

    Wire.requestFrom((uint8_t)CST9217_ADDR, (uint8_t)7);
    if (Wire.available() < 7) return false;

    uint8_t buf[7];
    for (int i = 0; i < 7; i++) buf[i] = Wire.read();

    // Write ACK (0xAB) back to 0xD000 to clear data
    Wire.beginTransmission(CST9217_ADDR);
    Wire.write((uint8_t)0xD0);
    Wire.write((uint8_t)0x00);
    Wire.write((uint8_t)0xAB);
    Wire.endTransmission();

    // Validate: buf[6] must be 0xAB, buf[0] must not be 0xAB
    if (buf[0] == 0xAB || buf[6] != 0xAB) return false;

    uint8_t num = buf[5] & 0x7F;
    if (num == 0 || num > 5) return false;

    // Event check: 0x06 = finger down
    uint8_t event = buf[0] & 0x0F;
    if (event != 0x06) return false;

    // 12-bit coordinates packed across 3 bytes, mirrored to match display
    x = 466 - ((buf[1] << 4) | (buf[3] >> 4));
    y = 466 - ((buf[2] << 4) | (buf[3] & 0x0F));

    if (x > 466 || y > 466) return false;
    return true;
}

static bool wasTouching = false;

void checkButtons() {
    if (digitalRead(PIN_BOOT) == LOW) {
        lookAt(0.7f, 0.7f);
    }

    int tx, ty;
    bool touching = readTouch(tx, ty);

    // Handle swipe gestures + panel
    handleSwipe(touching ? tx : 0, touching ? ty : 0, touching);

    // Only track eyes if panel is not visible
    if (touching && !panelVisible) {
        float fx = tx - 233, fy = ty - 233;
        float ca = cosf(-currentAngle), sa = sinf(-currentAngle);
        int rx = (int)(fx * ca - fy * sa) + 233;
        int ry = (int)(fx * sa + fy * ca) + 233;
        lookAtScreen(rx, ry);
    }

    wasTouching = touching;
}

// ═══════════════════════════════════════════════════════════
//  SETUP
// ═══════════════════════════════════════════════════════════

void setup() {
    Serial.begin(115200);
    delay(300);
    Serial.println("\n═══ CEREBRO FACE ═══");

    pinMode(PIN_BOOT, INPUT_PULLUP);

    // Display MUST init first (shares GPIO2 reset with touch)
    if (!canvas->begin()) {
        Serial.println("[DISPLAY] Canvas init failed!");
        while (1) delay(100);
    }
    Serial.println("[DISPLAY] Canvas OK (466x466 in PSRAM)");

    // Reset display rotation to 0 (we rotate pixels manually)
    display->setRotation(0);

    // Rotation buffer for pixel-level rotation
    rotBuf = (uint16_t *)heap_caps_malloc(466 * 466 * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
    if (rotBuf) Serial.println("[ROT] Buffer allocated");

    // Touch init after display (resets GPIO2 briefly)
    touchAvailable = touchInit();

    // IMU
    imuAvailable = imuInit();

    // Battery monitoring
    pmuAvailable = battInit();

    // BLE + Wi-Fi
    bleInit();

    // Audio (I2S + codecs + recording/playback)
    audioInit();

    // HTTP server + mDNS discovery (starts when WiFi connects)
    wifiServerInit();

    randomSeed(esp_random());
    morphInit(EXPR_NEUTRAL);
    setExpression(EXPR_NEUTRAL);

    Serial.println("[FACE] Ready — eyes tracking");
}

// ═══════════════════════════════════════════════════════════
//  LOOP
// ═══════════════════════════════════════════════════════════

void loop() {
    unsigned long now = millis();
    if (now - lastFrameTime < FRAME_MS) return;
    lastFrameTime = now;

    checkButtons();
    updateBlink();
    bleLoop();
    wifiServerLoop();

    // Map face code to expression (HTTP takes priority over demo cycle)
    int8_t fc = wifiGetFaceCode();
    if (fc >= 0) {
        ExpressionType expr = EXPR_NEUTRAL;
        switch (fc) {
            case 0x00: expr = EXPR_NEUTRAL;   break; // neutral
            case 0x01: expr = EXPR_HAPPY;     break; // happy
            case 0x02: expr = EXPR_THINKING;  break; // thinking
            case 0x03: expr = EXPR_SURPRISED; break; // surprised
            case 0x04: expr = EXPR_SAD;       break; // concerned
            case 0x05: expr = EXPR_HAPPY;     break; // excited (use happy)
            case 0x06: expr = EXPR_NEUTRAL;   break; // calm
            case 0x07: expr = EXPR_ANGRY;     break; // alert
            case 0x08: expr = EXPR_NEUTRAL;   break; // listening
            case 0x09: expr = EXPR_NEUTRAL;   break; // speaking
            case 0x0A: expr = EXPR_PENSIVE;   break; // sleeping
            default:   expr = EXPR_NEUTRAL;   break;
        }
        setExpression(expr);
    } else {
        // No app connected — demo cycle
        static unsigned long lastExprChange = 0;
        static int exprIndex = EXPR_NEUTRAL;
        if (now - lastExprChange > 5000) {
            lastExprChange = now;
            exprIndex = (exprIndex + 1) % EXPR_COUNT;
            setExpression((ExpressionType)exprIndex);
        }
    }

    drawFace();
}
