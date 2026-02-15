#include "MessageBox.h"
#include <pspgu.h>
#include <pspdisplay.h>
#include <pspctrl.h>
#include <pspkernel.h>
#include <string>
#include <vector>
#include <cstring>

// ---- time helper ----
static inline unsigned long long now_us() {
    // sceKernelGetSystemTimeWide returns microseconds since boot
    return sceKernelGetSystemTimeWide();
}

// Filled-rect helper (2-vertex sprite)
static void mbDrawRect(int x, int y, int w, int h, unsigned color) {
    struct V { unsigned color; short x, y, z; };
    V* v = (V*)sceGuGetMemory(2 * sizeof(V));
    v[0] = { color, (short)x,       (short)y,       0 };
    v[1] = { color, (short)(x + w), (short)(y + h), 0 };
    sceGuDisable(GU_TEXTURE_2D);
    sceGuShadeModel(GU_FLAT);
    sceGuAmbientColor(0xFFFFFFFF);
    sceGuDrawArray(GU_SPRITES,
                   GU_COLOR_8888 | GU_VERTEX_16BIT | GU_TRANSFORM_2D,
                   2, 0, v);
}

static void mbDrawTextureScaled(Texture* tex, float x, float y, float h, unsigned color) {
    if (!tex || !tex->data || tex->width <= 0 || tex->height <= 0 || h <= 0.0f) return;
    const float scale = h / (float)tex->height;
    const float w = (float)tex->width * scale;

    sceKernelDcacheWritebackRange(tex->data, tex->stride * tex->height * 4);
    sceGuTexFlush();

    sceGuEnable(GU_TEXTURE_2D);
    sceGuTexMode(GU_PSM_8888, 0, 0, GU_FALSE);
    sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGBA);
    sceGuTexImage(0, tex->stride, tex->stride, tex->stride, tex->data);
    sceGuTexFilter(GU_LINEAR, GU_LINEAR);
    sceGuTexWrap(GU_CLAMP, GU_CLAMP);

    const float u0 = 0.0f, v0 = 0.0f;
    const float u1 = (float)tex->width - 0.5f;
    const float v1 = (float)tex->height - 0.5f;

    struct V { float u, v; unsigned color; float x, y, z; };
    V* v = (V*)sceGuGetMemory(2 * sizeof(V));
    v[0] = { u0, v0, color, x,     y,     0.0f };
    v[1] = { u1, v1, color, x + w, y + h, 0.0f };
    sceGuDrawArray(GU_SPRITES, GU_TEXTURE_32BITF | GU_COLOR_8888 |
                              GU_VERTEX_32BITF  | GU_TRANSFORM_2D, 2, nullptr, v);
    sceGuDisable(GU_TEXTURE_2D);
}

static void mbDrawHFadeLine(int x, int y, int w, int h, unsigned char peakAlpha, int fadePx, unsigned rgbNoAlpha) {
    if (w <= 0 || h <= 0) return;
    if (fadePx < 1) fadePx = 1;
    for (int i = 0; i < w; ++i) {
        int left = i;
        int right = (w - 1) - i;
        int edgeDist = (left < right) ? left : right;
        float t = (edgeDist >= fadePx) ? 1.0f : ((float)edgeDist / (float)fadePx);
        unsigned a = (unsigned)(peakAlpha * t + 0.5f);
        if (a == 0) continue;
        mbDrawRect(x + i, y, 1, h, ((a & 0xFF) << 24) | (rgbNoAlpha & 0x00FFFFFF));
    }
}

static void mbCountLinesMaxChars(const char* s, int& outMaxChars, int& outLines) {
    outMaxChars = 0;
    outLines = 1;
    if (!s || !*s) return;
    int cur = 0;
    for (const char* p = s; *p; ++p) {
        if (*p == '\n') {
            if (cur > outMaxChars) outMaxChars = cur;
            cur = 0;
            ++outLines;
        } else {
            ++cur;
        }
    }
    if (cur > outMaxChars) outMaxChars = cur;
}

MessageBox::MessageBox(const char* message,
                       Texture* okIcon,
                       int screenW, int screenH,
                       float textScale,
                       int iconTargetH,
                       const char* okLabel,
                       int padX,   int padY,
                       int wrapTweakPx,
                       int forcedPxPerChar,
                       int panelW, int panelH,
                       unsigned closeButton)
: _msg(message),
  _okLabel(okLabel),
  _icon(okIcon),
  _screenW(screenW), _screenH(screenH),
  _x(0), _y(0), _w(panelW), _h(panelH),
  _padX(padX), _padY(padY),
  _wrapTweakPx(wrapTweakPx),
  _forcedPxPerChar(forcedPxPerChar),
  _textScale(textScale),
  _iconTargetH(iconTargetH),
  _visible(true),
  _closeButton(closeButton),
  _cancelButton(0),
  _canceled(false),
  _lastButtons(0),
  _holdStartUs(0),
  _holdMask(0),
  _armedForInput(true),
  _inputPrimed(false) {
    if (_w <= 0 || _h <= 0) {
        int maxChars = 0, lines = 1;
        mbCountLinesMaxChars(_msg, maxChars, lines);
        if (maxChars <= 0) maxChars = 1;
        int approxPxPerChar = (_forcedPxPerChar > 0)
                            ? _forcedPxPerChar
                            : (int)(14.0f * _textScale + 0.5f);
        if (approxPxPerChar < 6) approxPxPerChar = 6;
        const int lineH = (int)(24.0f * _textScale + 0.5f);
        const int textW = maxChars * approxPxPerChar;
        const int textH = lines * lineH;
        const int bottomAreaH = (_iconTargetH > 0 ? (_iconTargetH + 14) : 14);
        const int needW = textW + (_padX * 2);
        const int needH = _padY + textH + bottomAreaH + 6;
        if (_w <= 0) _w = needW;
        if (_h <= 0) _h = needH;
        const int maxW = _screenW - 20;
        const int maxH = _screenH - 20;
        if (_w > maxW) _w = maxW;
        if (_h > maxH) _h = maxH;
    }
    _x = (_screenW - _w) / 2;
    _y = (_screenH - _h) / 2;
}

bool MessageBox::update() {
    if (!_visible) return false;

    // --- Robust, non-blocking input sampling with debounce/hold fallback ---
    // Using "peek" avoids blocking frames and missing edges when heavy work happens elsewhere.
    SceCtrlData pad{}; 
    sceCtrlPeekBufferPositive(&pad, 1);
    unsigned now = pad.Buttons;

    // Per-instance input state: avoids cross-modal interference when multiple
    // MessageBox instances are updated in the same frame (e.g. msgBox + gUsbBox).
    if (!_inputPrimed) {
        _inputPrimed = true;
        _lastButtons = now;
        _holdStartUs = 0;
        _holdMask = 0;
        _armedForInput = false; // require one release after the modal appears
    }

    // Arm when the close button is fully released
    if (!_armedForInput) {
        unsigned gate = _closeButton | _cancelButton;
        if ((now & gate) == 0) {
            _armedForInput = true;
            _holdStartUs = 0;
            _holdMask = 0;
        }
        _lastButtons = now;
        return _visible;
    }

    // Edge detection + hold fallback (helps when edges are missed due to long frames)
    unsigned newlyPressed = now & ~_lastButtons;
    bool cancel = (_cancelButton != 0) && ((newlyPressed & _cancelButton) != 0);
    bool close = (!cancel) && ((newlyPressed & _closeButton) != 0);

    if (!close && !cancel) {
        unsigned curMask = 0;
        if (_cancelButton && (now & _cancelButton)) curMask = _cancelButton;
        else if (now & _closeButton) curMask = _closeButton;

        if (curMask == 0) {
            _holdStartUs = 0;
            _holdMask = 0;
        } else {
            if (curMask != _holdMask) {
                _holdMask = curMask;
                _holdStartUs = now_us();
            } else {
                if (_holdStartUs == 0) _holdStartUs = now_us();
                unsigned long long elapsed = now_us() - _holdStartUs;
                // 220ms hold fallback: feels snappy but forgiving
                if (elapsed >= 220000ULL) {
                    if (_holdMask == _cancelButton) cancel = true;
                    else if (_holdMask == _closeButton) close = true;
                }
            }
        }
    }

    _lastButtons = now;

    if (close || cancel) {
        _canceled = cancel;
        _visible = false;
        // If this instance is reused later, require a fresh release gate.
        _armedForInput = false;
        _inputPrimed = false;
    }
    return _visible;
}



// very simple char-based wrapper with a better px/char estimate
static void wrapTextByChars(const char* text, int maxCharsPerLine, std::vector<std::string>& outLines) {
    outLines.clear();
    if (!text || !*text) return;

    const char* s = text;
    while (*s) {
        int take = 0, lastSpace = -1;
        while (s[take] && take < maxCharsPerLine) {
            if (s[take] == ' ') lastSpace = take;
            if (s[take] == '\n') { lastSpace = take; break; }
            ++take;
        }
        int cut = take;
        if (s[cut] == '\n') {
            // break at newline
        } else if (take == maxCharsPerLine && lastSpace >= 0) {
            cut = lastSpace; // wrap at last space
        }

        outLines.emplace_back(std::string(s, s + cut));

        if (s[cut] == '\n')      s += cut + 1;
        else if (cut == 0)       s += 1;            // force progress on long word
        else                     s += (cut < take ? cut + 1 : cut);

        while (*s == ' ') ++s;   // trim leading spaces for next line
    }
}

void MessageBox::render(intraFont* font) {
    if (!_visible) return;

    // Activate font to properly bind font texture to GPU
    if (font) intraFontActivate(font);

    // Dim overlay
    sceGuDisable(GU_DEPTH_TEST);
    sceGuEnable(GU_BLEND);
    sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0);
    mbDrawRect(0, 0, _screenW, _screenH, 0x88000000);

    // Panel + border
    const unsigned COLOR_PANEL    = 0xD0303030;
    const unsigned COLOR_BORDER   = 0xFFFFFFFF;
    const unsigned COLOR_TEXT     = 0xFFFFFFFF;
    const unsigned PROG_BAR_BG    = 0xFF666666;
    const unsigned PROG_BAR_FILL  = 0xFFFFFFFF; // white fill; simple and readable

    mbDrawRect(_x - 1, _y - 1, _w + 2, _h + 2, COLOR_BORDER);
    mbDrawRect(_x, _y, _w, _h, COLOR_PANEL);

    // Content box (respect padding; padY lowers the first line)
    const int innerX = _x + _padX;
    const int innerY = _y + _padY;
    const int innerW = _w - 2 * _padX;

    // px/char estimate (override if forcedPxPerChar > 0)
    int approxPxPerChar = (_forcedPxPerChar > 0)
                        ? _forcedPxPerChar
                        : (int)(14.0f * _textScale + 0.5f);
    if (approxPxPerChar < 6) approxPxPerChar = 6;
    // Give some slack so it wraps later if desired
    int maxChars = (innerW + _wrapTweakPx) / approxPxPerChar;
    if (maxChars < 8) maxChars = 8;

    // ---- Title / message (existing) ----
    std::vector<std::string> lines;
    wrapTextByChars(_msg ? _msg : "", maxChars, lines);

    int y = innerY + (_progEnabled ? 2 : 0);
    if (font) {
        const float titleScale = _textScale;
        const float subtitleScale = (_useSubtitleStyle && _subtitleScale > 0.0f)
                                    ? _subtitleScale : _textScale;
        const unsigned titleColor = COLOR_TEXT;
        const unsigned subtitleColor = _useSubtitleStyle ? _subtitleColor : COLOR_TEXT;
        auto lineHFor = [](float scale) {
            return (int)(24.0f * scale + 0.5f);
        };

        // reserve bottom area for icon/OK OR for the progress bar, whichever is larger
        const int iconTargetH = (_animFrames && _animCount > 0 && _animTargetH > 0) ? _animTargetH : _iconTargetH;
        const int titleLineH = lineHFor(titleScale);
        const int bottomAreaH = (_progEnabled ? (titleLineH + 16 /*filename*/ + 10 /*gap*/ + 4 /*bar*/) : (iconTargetH + _okBottomPad));
        const int maxY = _y + _h - bottomAreaH - 6;

        for (size_t i = 0; i < lines.size(); ++i) {
            const bool isTitle = (i == 0);
            const float scale = isTitle ? titleScale : subtitleScale;
            const unsigned color = isTitle ? titleColor : subtitleColor;
            intraFontSetStyle(font, scale, color, 0, 0.0f, INTRAFONT_ALIGN_LEFT);
            const char* raw = lines[i].c_str();
            const char* text = raw;
            bool hadToken = false;
            if (_inlineToken) {
                size_t tokLen = std::strlen(_inlineToken);
                if (tokLen > 0 && std::strncmp(raw, _inlineToken, tokLen) == 0 && raw[tokLen] == ' ') {
                    text = raw + tokLen + 1;
                    hadToken = true;
                }
            }
            float textX = (float)innerX;
            float textY = (float)y;
            if (hadToken) {
                float iconH = 12.0f * (scale / 0.7f);
                if (iconH < 8.0f) iconH = 8.0f;
                if (_inlineIcon && _inlineIcon->data) {
                    float iconY = (float)y - iconH + (float)(22.0f * scale * 0.75f) - 5.0f;
                    mbDrawTextureScaled(_inlineIcon, textX, iconY, iconH, 0xFFFFFFFF);
                    float iconW = (float)_inlineIcon->width * (iconH / (float)_inlineIcon->height);
                    textX += iconW + 4.0f;
                }
                textY += 5.0f;
            }
            intraFontPrint(font, textX, textY, text);
            int lineH = lineHFor(scale);
            if (isTitle && _useSubtitleStyle) {
                lineH += _subtitleGapAdjust;
                if (lineH < 8) lineH = 8;
            }
            y += lineH;
            if (y > maxY) break;
        }
    }

    // ---- Progress UI ----
    if (_progEnabled) {
        const int sepW = _w - 6;
        if (sepW > 0) {
            mbDrawHFadeLine(_x + 3, y - 16, sepW, 1, 0xA0, 20, 0x00C0C0C0);
            y += 7;
        }

        const int barH = 12;
        const int barW = innerW;
        const int barX = innerX;
        const float textMaxW = (float)barW;
        if (font) {
            float cx = _x + _w * 0.5f;
            const float titleScale = _textScale * 0.85f;
            const float detailScale = (_useSubtitleStyle && _subtitleScale > 0.0f)
                                      ? _subtitleScale : 0.7f;
            const unsigned detailColor = _useSubtitleStyle ? _subtitleColor : 0xFFBBBBBB;

            auto trimToWidth = [&](const std::string& s, float scale, float maxW)->std::string {
                if (s.empty() || maxW <= 0.0f) return s;
                if (!font) {
                    const float pxPerChar = 12.0f * scale;
                    int maxChars = (pxPerChar > 0.0f) ? (int)(maxW / pxPerChar) : 0;
                    if (maxChars <= 0) return std::string();
                    if ((int)s.size() <= maxChars) return s;
                    if (maxChars <= 3) return s.substr(0, maxChars);
                    return s.substr(0, maxChars - 3) + "...";
                }
                intraFontSetStyle(font, scale, COLOR_TEXT, 0, 0.0f, INTRAFONT_ALIGN_LEFT);
                if (intraFontMeasureText(font, s.c_str()) <= maxW) return s;
                const char* ell = "...";
                const float ellW = intraFontMeasureText(font, ell);
                if (ellW > maxW) return std::string();
                size_t lo = 0, hi = s.size();
                while (lo < hi) {
                    size_t mid = (lo + hi) / 2;
                    std::string cand = s.substr(0, mid) + ell;
                    if (intraFontMeasureText(font, cand.c_str()) <= maxW) lo = mid + 1;
                    else hi = mid;
                }
                size_t fit = (lo == 0) ? 0 : (lo - 1);
                std::string out = s.substr(0, fit);
                out += ell;
                return out;
            };

            // Line 1: app title (headline), if any
            if (!_progTitle.empty()) {
                std::string t = trimToWidth(_progTitle, titleScale, textMaxW);
                float cy = (float)(y + 6);
                intraFontSetStyle(font, titleScale, COLOR_TEXT, 0, 0.0f, INTRAFONT_ALIGN_CENTER);
                intraFontPrint(font, cx, cy, t.c_str());
                y = (int)(cy + (int)(22.0f * titleScale + 0.5f));
            }

            // Line 2: filename (detail), if any
            if (_progDetailVisible && !_progDetail.empty()) {
                std::string d = trimToWidth(_progDetail, detailScale, textMaxW);
                float cy = (float)(y + 2);
                intraFontSetStyle(font, detailScale, detailColor, 0, 0.0f, INTRAFONT_ALIGN_CENTER);
                intraFontPrint(font, cx, cy, d.c_str());
                y = (int)(cy + (int)(22.0f * detailScale + 0.5f));
            } else {
                // keep some breathing room even if no detail
                y += (int)(16.0f * _textScale + 0.5f);
            }
        }

        // progress bar
        int barY = y + 6;

        mbDrawRect(barX, barY, barW, barH, PROG_BAR_BG);

        // Fill
        uint64_t sz = (_progSize == 0) ? 1 : _progSize;
        uint64_t off = _progOffset;
        if (off > sz) off = sz;
        int fillW = (int)((barW * (double)off) / (double)sz + 0.5);
        if (fillW > 0) mbDrawRect(barX, barY, fillW, barH, PROG_BAR_FILL);

        y = barY + barH + 6;
    }


    // ---- Original bottom icon + "OK" (only when not in progress mode) ----
    Texture* iconTex = _icon;
    float iconH = (float)_iconTargetH;
    if (_animFrames && _animCount > 0) {
        unsigned long long now = now_us();
        if (_animNextUs == 0) {
            _animIndex = 0;
            _animNextUs = now + (unsigned long long)_animFrames[_animIndex].delayMs * 1000ULL;
        } else if (now >= _animNextUs) {
            _animIndex = (_animIndex + 1) % _animCount;
            _animNextUs = now + (unsigned long long)_animFrames[_animIndex].delayMs * 1000ULL;
        }
        iconTex = _animFrames[_animIndex].tex;
        if (_animTargetH > 0) iconH = (float)_animTargetH;
    }
    float drawW = 0.0f;

    if (iconTex && iconTex->data && iconTex->height > 0) {
        float scale = iconH / (float)iconTex->height;
        drawW = iconTex->width * scale;
    }

    // Approx text width for short labels like "OK"
    float okScale = (_okScale > 0.0f) ? _okScale : _textScale;
    float fontPx = 24.0f * okScale;
    auto measureTextW = [&](const char* s)->float {
        if (!s || !*s) return 0.0f;
        if (font) {
            intraFontSetStyle(font, okScale, COLOR_TEXT, 0, 0.0f, INTRAFONT_ALIGN_LEFT);
            return intraFontMeasureText(font, s);
        }
        return (float)std::strlen(s) * (fontPx * 0.55f);
    };
    float okTextW = measureTextW(_okLabel ? _okLabel : "OK");
    float gap = 6.0f;
    float groupGap = 12.0f;

    // Cancel measurements (optional)
    Texture* cancelTex = _cancelIcon;
    float cancelDrawW = 0.0f;
    if (cancelTex && cancelTex->data && cancelTex->height > 0 && iconH > 0.0f) {
        float scale = iconH / (float)cancelTex->height;
        cancelDrawW = cancelTex->width * scale;
    }
    float cancelTextW = measureTextW(_cancelLabel ? _cancelLabel : "");

    const bool showOk = ((_okLabel && *_okLabel) || drawW > 0.0f);
    const bool showCancel = ((_cancelLabel && *_cancelLabel) || cancelDrawW > 0.0f);
    float okGroupW = showOk ? (drawW + (drawW > 0 ? gap : 0) + okTextW) : 0.0f;
    float cancelGroupW = showCancel ? (cancelDrawW + (cancelDrawW > 0 ? gap : 0) + cancelTextW) : 0.0f;

    // Center the whole (ok + cancel) group horizontally
    float totalW = okGroupW + ((showOk && showCancel) ? groupGap : 0.0f) + cancelGroupW;
    int okLeftPad = (_okLeftPad >= 0) ? _okLeftPad : _padX;
    float startX = _okAlignLeft ? (float)(_x + okLeftPad) : (_x + (_w - totalW) * 0.5f);

    // Baseline Y so text vertically centers against the icon
    const int bottomAreaTop = _y + _h - (int)(iconH + _okBottomPad);
    float iy0 = (float)bottomAreaTop;
    float iy1 = iy0 + iconH;
    float textBaseline = iy0 + iconH * 0.5f + fontPx * 0.35f; // nudge factor

    // OK icon + label
    float okStartX = startX;
    if (showOk) {
        if (iconTex && iconTex->data && drawW > 0.0f) {
            sceGuEnable(GU_TEXTURE_2D);
            sceGuTexMode(GU_PSM_8888, 0, 0, GU_FALSE);
            sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGBA);
            sceGuTexImage(0, iconTex->stride, iconTex->stride, iconTex->stride, iconTex->data);
            sceGuTexFilter(GU_NEAREST, GU_NEAREST);
            sceGuTexWrap(GU_CLAMP, GU_CLAMP);

            float ix0 = okStartX;
            float ix1 = ix0 + drawW;

            struct V { float u, v; unsigned color; float x, y, z; };
            V* v = (V*)sceGuGetMemory(2 * sizeof(V));
            v[0].u = 0.0f;                 v[0].v = 0.0f;
            v[0].x = ix0;                  v[0].y = iy0;  v[0].z = 0.0f; v[0].color = 0xFFFFFFFF;
            v[1].u = (float)iconTex->width  - 0.5f;
            v[1].v = (float)iconTex->height - 0.5f;
            v[1].x = ix1;                  v[1].y = iy1;  v[1].z = 0.0f; v[1].color = 0xFFFFFFFF;

            sceGuDrawArray(GU_SPRITES,
                           GU_TEXTURE_32BITF | GU_VERTEX_32BITF |
                           GU_COLOR_8888    | GU_TRANSFORM_2D,
                           2, nullptr, v);

            sceGuDisable(GU_TEXTURE_2D);

            if (font && _okLabel && *_okLabel) {
                intraFontSetStyle(font, okScale, _okColor, 0, 0.0f, INTRAFONT_ALIGN_LEFT);
                float okX = ix1 + gap + _okTextOffsetX;
                float okY = textBaseline + _okTextOffsetY;
                intraFontPrint(font, okX, okY, _okLabel ? _okLabel : "OK");
            }
        } else if (font && _okLabel && *_okLabel) {
            intraFontSetStyle(font, okScale, _okColor, 0, 0.0f, INTRAFONT_ALIGN_LEFT);
            float okX = okStartX + _okTextOffsetX;
            float okY = textBaseline + _okTextOffsetY;
            intraFontPrint(font, okX, okY, _okLabel ? _okLabel : "OK");
        }
    }

    // Cancel icon + label
    if (showCancel) {
        float cancelStartX = okStartX + okGroupW + (showOk ? groupGap : 0.0f);
        if (cancelTex && cancelTex->data && cancelDrawW > 0.0f) {
            sceGuEnable(GU_TEXTURE_2D);
            sceGuTexMode(GU_PSM_8888, 0, 0, GU_FALSE);
            sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGBA);
            sceGuTexImage(0, cancelTex->stride, cancelTex->stride, cancelTex->stride, cancelTex->data);
            sceGuTexFilter(GU_NEAREST, GU_NEAREST);
            sceGuTexWrap(GU_CLAMP, GU_CLAMP);

            float ix0 = cancelStartX;
            float ix1 = ix0 + cancelDrawW;

            struct V { float u, v; unsigned color; float x, y, z; };
            V* v = (V*)sceGuGetMemory(2 * sizeof(V));
            v[0].u = 0.0f;                  v[0].v = 0.0f;
            v[0].x = ix0;                   v[0].y = iy0;  v[0].z = 0.0f; v[0].color = 0xFFFFFFFF;
            v[1].u = (float)cancelTex->width  - 0.5f;
            v[1].v = (float)cancelTex->height - 0.5f;
            v[1].x = ix1;                   v[1].y = iy1;  v[1].z = 0.0f; v[1].color = 0xFFFFFFFF;

            sceGuDrawArray(GU_SPRITES,
                           GU_TEXTURE_32BITF | GU_VERTEX_32BITF |
                           GU_COLOR_8888    | GU_TRANSFORM_2D,
                           2, nullptr, v);

            sceGuDisable(GU_TEXTURE_2D);

            if (font && _cancelLabel && *_cancelLabel) {
                intraFontSetStyle(font, okScale, _okColor, 0, 0.0f, INTRAFONT_ALIGN_LEFT);
                float cx = ix1 + gap + _okTextOffsetX;
                float cy = textBaseline + _okTextOffsetY;
                intraFontPrint(font, cx, cy, _cancelLabel);
            }
        } else if (font && _cancelLabel && *_cancelLabel) {
            intraFontSetStyle(font, okScale, _okColor, 0, 0.0f, INTRAFONT_ALIGN_LEFT);
            float cx = cancelStartX + _okTextOffsetX;
            float cy = textBaseline + _okTextOffsetY;
            intraFontPrint(font, cx, cy, _cancelLabel);
        }
    }
    if (!showOk && showCancel) {
        // If only cancel is shown, keep its label centered when not left-aligned.
        if (!_okAlignLeft && font && _cancelLabel && *_cancelLabel && cancelDrawW <= 0.0f) {
            intraFontSetStyle(font, okScale, _okColor, 0, 0.0f, INTRAFONT_ALIGN_CENTER);
            float cx = _x + _w * 0.5f + _okTextOffsetX;
            float cy = textBaseline + _okTextOffsetY;
            intraFontPrint(font, cx, cy, _cancelLabel);
        }
    }
}

void MessageBox::setAnimation(const MBAnimFrame* frames, size_t count, int targetH) {
    _animFrames = frames;
    _animCount = count;
    _animIndex = 0;
    _animNextUs = 0;
    _animTargetH = targetH;
}

void MessageBox::setOkStyle(float scale, unsigned color) {
    _okScale = scale;
    _okColor = color;
}

void MessageBox::setOkAlignLeft(bool left) {
    _okAlignLeft = left;
}

void MessageBox::setOkPosition(int leftPadPx, int bottomPadPx) {
    _okLeftPad = leftPadPx;
    _okBottomPad = bottomPadPx;
}

void MessageBox::setOkTextOffset(int dx, int dy) {
    _okTextOffsetX = dx;
    _okTextOffsetY = dy;
}

void MessageBox::setSubtitleStyle(float scale, unsigned color) {
    _subtitleScale = scale;
    _subtitleColor = color;
    _useSubtitleStyle = true;
}

void MessageBox::setSubtitleGapAdjust(int px) {
    _subtitleGapAdjust = px;
}

void MessageBox::setInlineIcon(Texture* icon, const char* token) {
    _inlineIcon = icon;
    _inlineToken = token;
}

void MessageBox::setCancel(Texture* icon, const char* label, unsigned button) {
    _cancelIcon = icon;
    _cancelLabel = label;
    _cancelButton = button;
    _canceled = false;
}

// ---- New: progress API impl ----
void MessageBox::showProgress(const char* fileMessage, uint64_t offset, uint64_t size) {
    _progEnabled = true;
    _progDetail  = (fileMessage ? fileMessage : "");
    _progOffset  = offset;
    _progSize    = (size == 0) ? 1 : size;
}

void MessageBox::updateProgress(uint64_t offset, uint64_t size, const char* fileMessage) {
    _progOffset = offset;
    _progSize   = (size == 0) ? 1 : size;
    if (fileMessage) _progDetail = fileMessage;
}

void MessageBox::setProgressTitle(const char* title) {
    _progTitle = title ? title : "";
    _progEnabled = true; // ensure progress UI shows when we set a title first
}

void MessageBox::setProgressDetailVisible(bool visible) {
    _progDetailVisible = visible;
}

void MessageBox::setMessage(const char* message) {
    _msg = message ? message : "";
}

void MessageBox::hideProgress() {
    _progEnabled = false;
    _progTitle.clear();
    _progDetail.clear();
    _progDetailVisible = true;
    _progOffset = 0;
    _progSize   = 1;
}
