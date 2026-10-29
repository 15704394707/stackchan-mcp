// face_service.cpp
// AnimatedGIF-based face expression system
// Replaces static PNG display with looping GIF animations
//
// 2026-10-29 追加：底部中文字幕条（showTextOverlay / clearTextOverlay）
// 布局：字幕出现时，GIF 从 1.25 倍缩到 1.0 倍（192x192）贴顶显示，
//       屏幕底部 48px 留给中文。GIF 继续动，不会覆盖字幕区。

#include "face_service.h"
#include "gif_assets.h"
#include <AnimatedGIF.h>
#include <M5Unified.h>
#include <SPIFFS.h>
#include <atomic>
#include <cstring>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

// ── GIF engine state ───────────────────────────────────────────────────────
static AnimatedGIF gif;
static const uint8_t* current_gif_data   = nullptr;
static size_t         current_gif_len    = 0;
static volatile bool  gif_changed        = false;
static int            gif_src_width      = 0;
static int            gif_src_height     = 0;
static SemaphoreHandle_t display_mutex    = nullptr;

// The HTTP server runs on the Arduino loop task while GIF rendering runs on
// core 1. A short preview gate and display mutex keep both from drawing at
// once, then force the newest face state to be redrawn when the preview ends.
static std::atomic<bool>     camera_preview_active{false};
static std::atomic<uint32_t> camera_preview_until_ms{0};

// ── 底部字幕条状态（新增）────────────────────────────────────────────────
static std::atomic<bool>     subtitle_mode{false};      // true = 脸缩到顶部 192px，底部留字幕区
static std::atomic<bool>     text_overlay_active{false};
static std::atomic<uint32_t> text_overlay_until_ms{0};
static char                  text_overlay_buf[512];

#define SUBTITLE_AREA_Y   192      // 字幕区起始 y
#define SUBTITLE_AREA_H   48       // 字幕区高度
#define SUBTITLE_LINE_H   20
#define SUBTITLE_MAX_LINES 2

// ── Face tracking ──────────────────────────────────────────────────────────
static WhaleFace currentFace = WHALE_CALM;
static bool      isTalking   = false;
static uint32_t  faceCommandRevision = 0;

// ── GIF asset table ────────────────────────────────────────────────────────
struct GifAsset { const uint8_t* data; size_t len; };

static const GifAsset GIF_ASSETS[] = {
    { face_gif_calm,    face_gif_calm_len    },  // WHALE_CALM    = 0
    { face_gif_thinking,face_gif_thinking_len},  // WHALE_THINKING = 1
    { face_gif_happy,   face_gif_happy_len   },  // WHALE_HAPPY   = 2
    { face_gif_sleepy,  face_gif_sleepy_len  },  // WHALE_SLEEPY  = 3
    { face_gif_shy,     face_gif_shy_len     },  // WHALE_SHY     = 4
    { face_gif_smug,    face_gif_smug_len    },  // WHALE_SMUG    = 5
    { face_gif_pouty,   face_gif_pouty_len   },  // WHALE_POUTY   = 6
};
static const int NUM_GIF_ASSETS = 7;

// ── GIFDraw callback ───────────────────────────────────────────────────────
// Called once per scanline of each frame.
// GIF is 192x192. Normal mode: 1.25x -> 240x240, centered (fills screen).
// Subtitle mode: 1.0x -> 192x192, pinned to top, leaving the bottom 48px free.
// Color note: ILI9342 (CoreS3 LCD) is configured in BGR mode via MADCTL.
// We swap the R and B fields of the RGB565_LE palette value before buffering.
#define SCALE_NUM 5
#define SCALE_DEN 4

static bool previewDeadlineReached(uint32_t now, uint32_t deadline) {
    return static_cast<int32_t>(now - deadline) >= 0;
}

static bool takeDisplay(TickType_t timeout) {
    return display_mutex == nullptr || xSemaphoreTake(display_mutex, timeout) == pdTRUE;
}

static void releaseDisplay() {
    if (display_mutex != nullptr) {
        xSemaphoreGive(display_mutex);
    }
}

static void GIFDraw(GIFDRAW* pDraw) {
    int srcW = pDraw->iWidth;
    int srcH = gif_src_height;

    const bool sub = subtitle_mode.load();
    const int sn = sub ? 4 : SCALE_NUM;   // 4/4 = 1.0x, 5/4 = 1.25x
    const int sd = SCALE_DEN;

    int dstW = srcW * sn / sd;
    int dstH = srcH * sn / sd;

    int xOff = (320 - dstW) / 2;
    if (xOff < 0) xOff = 0;
    int yOff = sub ? 0 : (240 - dstH) / 2;
    if (yOff < 0) yOff = 0;

    int srcY = pDraw->iY + pDraw->y;

    uint8_t*  src     = pDraw->pPixels;
    uint16_t* palette = pDraw->pPalette;
    bool      hasTrans = (pDraw->ucHasTransparency != 0);
    uint8_t   transIdx = pDraw->ucTransparent;

    uint16_t lineBuf[320];
    int outW = (dstW <= 320) ? dstW : 320;

    for (int dx = 0; dx < outW; dx++) {
        int sx = dx * sd / sn;
        if (sx >= srcW) sx = srcW - 1;
        uint8_t idx = src[sx];
        if (hasTrans && idx == transIdx) {
            lineBuf[dx] = 0x0000;
        } else {
            lineBuf[dx] = palette[idx];
        }
    }

    int destY_start = yOff + srcY * sn / sd;
    int destY_end   = yOff + (srcY + 1) * sn / sd;

    M5.Display.setSwapBytes(true);
    for (int dy = destY_start; dy < destY_end && dy < 240; dy++) {
        if (dy >= 0) {
            M5.Display.pushImage(xOff, dy, outW, 1, lineBuf);
        }
    }
    M5.Display.setSwapBytes(false);
}

// ── 底部字幕绘制（新增）──────────────────────────────────────────────────
static int utf8SeqLen(unsigned char c) {
    if (c < 0x80) return 1;
    if ((c & 0xE0) == 0xC0) return 2;
    if ((c & 0xF0) == 0xE0) return 3;
    if ((c & 0xF8) == 0xF0) return 4;
    return 1;
}

// 只画底部 48px 区域：一条分隔线 + 最多两行 16px 中文。
static void drawSubtitleBar(const char* text) {
    M5.Display.fillRect(0, SUBTITLE_AREA_Y, 320, SUBTITLE_AREA_H, TFT_BLACK);
    M5.Display.drawFastHLine(0, SUBTITLE_AREA_Y, 320,
                             M5.Display.color565(70, 55, 62));

    M5.Display.setTextSize(1);
    M5.Display.setFont(&fonts::efontCN_16);
    M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);
    M5.Display.setTextDatum(TL_DATUM);

    const int x0    = 10;
    const int maxW  = 300;
    const int y0    = SUBTITLE_AREA_Y + 3;
    const int lineH = SUBTITLE_LINE_H;
    const int maxLines = SUBTITLE_MAX_LINES;

    int cx = x0;
    int cy = y0;
    int lines = 1;
    const char* p = text;

    while (*p != 0) {
        if (*p == '\n') {
            cx = x0;
            cy += lineH;
            ++lines;
            ++p;
            if (lines > maxLines) break;
            continue;
        }

        int len = utf8SeqLen(static_cast<unsigned char>(*p));
        int w = (static_cast<unsigned char>(*p) < 0x80) ? 8 : 16;

        if (cx + w > x0 + maxW) {
            cx = x0;
            cy += lineH;
            ++lines;
            if (lines > maxLines) break;
        }

        char one[8];
        if (len > 7) len = 7;
        memcpy(one, p, len);
        one[len] = 0;
        M5.Display.drawString(one, cx, cy);

        cx += w;
        p += len;
    }
}

// ── Animation task ─────────────────────────────────────────────────────────
static void faceTask(void* param) {
    gif.begin(GIF_PALETTE_RGB565_LE);

    while (true) {
        if (camera_preview_active.load()) {
            const uint32_t deadline = camera_preview_until_ms.load();
            if (!previewDeadlineReached(millis(), deadline)) {
                vTaskDelay(pdMS_TO_TICKS(25));
                continue;
            }

            camera_preview_active.store(false);
            gif_changed = true;
        }

        // 字幕超时：只撤掉字幕条，脸继续动，不中断 GIF
        if (text_overlay_active.load()) {
            const uint32_t deadline = text_overlay_until_ms.load();
            if (previewDeadlineReached(millis(), deadline)) {
                text_overlay_active.store(false);
                subtitle_mode.store(false);
                if (takeDisplay(pdMS_TO_TICKS(500))) {
                    M5.Display.fillScreen(TFT_BLACK);
                    releaseDisplay();
                }
                gif_changed = true;
                Serial.println("[SPEECH] Subtitle expired, face restored");
            }
        }

        if (gif_changed) {
            gif.close();

            if (!takeDisplay(portMAX_DELAY)) {
                vTaskDelay(pdMS_TO_TICKS(25));
                continue;
            }

            if (camera_preview_active.load()) {
                releaseDisplay();
                continue;
            }

            // 字幕态下不清屏，否则会把底部字幕擦掉
            if (!subtitle_mode.load()) {
                M5.Display.fillScreen(TFT_BLACK);
            }

            if (current_gif_data != nullptr) {
                int ok = gif.openFLASH((uint8_t*)current_gif_data,
                                       (int)current_gif_len,
                                       GIFDraw);
                if (ok) {
                    gif_src_width  = gif.getCanvasWidth();
                    gif_src_height = gif.getCanvasHeight();
                    Serial.printf("[FACE] GIF opened: %dx%d\n",
                                  gif_src_width, gif_src_height);
                } else {
                    Serial.printf("[FACE] GIF open failed (err %d)\n",
                                  gif.getLastError());
                }
            }
            gif_changed = false;
            releaseDisplay();
        }

        if (current_gif_data != nullptr) {
            if (!takeDisplay(portMAX_DELAY)) {
                vTaskDelay(pdMS_TO_TICKS(25));
                continue;
            }

            if (camera_preview_active.load()) {
                releaseDisplay();
                continue;
            }

            int frameResult = gif.playFrame(true, nullptr);
            if (frameResult == 0) {
                gif.reset();
            }
            releaseDisplay();
        } else {
            vTaskDelay(pdMS_TO_TICKS(50));
        }
    }
}

// ── Internal helpers ───────────────────────────────────────────────────────
static void switchToFace(WhaleFace face) {
    if (face < 0 || face >= NUM_GIF_ASSETS) face = WHALE_CALM;
    currentFace      = face;
    current_gif_data = GIF_ASSETS[face].data;
    current_gif_len  = GIF_ASSETS[face].len;
    gif_changed      = true;
}

// ── Public API ─────────────────────────────────────────────────────────────

void initFace() {
    if (!SPIFFS.begin(true)) {
        Serial.println("[FACE] SPIFFS mount failed!");
    } else {
        Serial.println("[FACE] SPIFFS mounted");
    }

    current_gif_data = GIF_ASSETS[WHALE_CALM].data;
    current_gif_len  = GIF_ASSETS[WHALE_CALM].len;
    gif_changed      = true;
    currentFace      = WHALE_CALM;

    display_mutex = xSemaphoreCreateMutex();
    if (display_mutex == nullptr) {
        Serial.println("[FACE] Display mutex allocation failed; camera preview disabled");
    }

    xTaskCreatePinnedToCore(faceTask, "face_gif", 8192, nullptr, 1, nullptr, 1);

    Serial.println("[FACE] GIF face service started");
    Serial.printf("[FACE] Free heap: %u  Free PSRAM: %u\n",
                  ESP.getFreeHeap(), ESP.getFreePsram());
}

void setFaceExpression(FaceExpression expr) {
    WhaleFace target;

    switch (expr) {
        case FACE_IDLE:      target = WHALE_CALM;     isTalking = false; break;
        case FACE_LISTENING: target = WHALE_THINKING; isTalking = false; break;
        case FACE_PLAYING:   target = WHALE_HAPPY;    isTalking = true;  break;
        case FACE_THINKING:  target = WHALE_THINKING; isTalking = false; break;
        case FACE_HAPPY:     target = WHALE_HAPPY;    isTalking = false; break;
        default:             target = WHALE_CALM;     isTalking = false; break;
    }

    ++faceCommandRevision;
    if (target != currentFace) {
        switchToFace(target);
    }
}

void setMouthOpen(float ratio) {
    (void)ratio;
}

void setWhaleFace(WhaleFace face) {
    isTalking = false;
    ++faceCommandRevision;
    switchToFace(face);
}

const char* getCurrentFaceName() {
    return whaleFaceName(currentFace);
}

uint32_t getFaceCommandRevision() {
    return faceCommandRevision;
}

bool showCameraPreview(const uint8_t* jpegData, size_t jpegLen, uint32_t durationMs) {
    if (jpegData == nullptr || jpegLen == 0 || display_mutex == nullptr) {
        return false;
    }

    camera_preview_until_ms.store(millis() + durationMs);
    camera_preview_active.store(true);

    if (!takeDisplay(pdMS_TO_TICKS(2000))) {
        camera_preview_active.store(false);
        gif_changed = true;
        Serial.println("[FACE] Camera preview timed out waiting for display");
        return false;
    }

    const bool drawn = M5.Display.drawJpg(
        jpegData,
        static_cast<uint32_t>(jpegLen),
        0, 0,
        M5.Display.width(),
        M5.Display.height());
    releaseDisplay();

    if (!drawn) {
        camera_preview_active.store(false);
        gif_changed = true;
        Serial.println("[FACE] Camera preview JPEG decode failed");
        return false;
    }

    Serial.printf("[FACE] Camera preview shown for %u ms\n", (unsigned)durationMs);
    return true;
}

// ── 底部字幕条（新增）────────────────────────────────────────────────────

bool showTextOverlay(const char* utf8Text, uint32_t durationMs) {
    if (utf8Text == nullptr || display_mutex == nullptr) {
        return false;
    }

    strncpy(text_overlay_buf, utf8Text, sizeof(text_overlay_buf) - 1);
    text_overlay_buf[sizeof(text_overlay_buf) - 1] = 0;

    const uint32_t holdMs = (durationMs == 0) ? 0x7FFFFFFFu : durationMs;
    text_overlay_until_ms.store(millis() + holdMs);

    const bool wasSub = subtitle_mode.load();
    subtitle_mode.store(true);
    text_overlay_active.store(true);

    if (!takeDisplay(pdMS_TO_TICKS(2000))) {
        subtitle_mode.store(wasSub);
        text_overlay_active.store(false);
        Serial.println("[SPEECH] Subtitle timed out waiting for display");
        return false;
    }

    if (!wasSub) {
        // 首次进入字幕态：布局从 240x240 变 192x192，整屏清一次
        M5.Display.fillScreen(TFT_BLACK);
    }
    drawSubtitleBar(text_overlay_buf);
    releaseDisplay();

    Serial.printf("[SPEECH] Subtitle: %u bytes, %u ms\n",
                  (unsigned)strlen(text_overlay_buf), (unsigned)holdMs);
    return true;
}

void clearTextOverlay() {
    text_overlay_active.store(false);
    subtitle_mode.store(false);

    if (takeDisplay(pdMS_TO_TICKS(1000))) {
        M5.Display.fillScreen(TFT_BLACK);
        releaseDisplay();
    }
    gif_changed = true;
    Serial.println("[SPEECH] Subtitle cleared, face restored");
}

bool isTextOverlayActive() {
    return text_overlay_active.load();
}
