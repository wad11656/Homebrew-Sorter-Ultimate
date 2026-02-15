// include/MessageBox.h
#ifndef MESSAGEBOX_H
#define MESSAGEBOX_H

#include <pspctrl.h>
#include <intraFont.h>
#include <string>
#include <cstddef>
#include "Texture.h"

struct MBAnimFrame {
    Texture* tex;
    uint32_t delayMs;
};

class MessageBox {
    public:
        // New closeButton param (defaults to CROSS to preserve existing behavior)
        MessageBox(const char* message,
                Texture* okIcon,
                int screenW = 480, int screenH = 272,
                float textScale = 0.9f,
                int iconTargetH = 22,
                const char* okLabel = "OK",
                int padX = 16, int padY = 26,
                int wrapTweakPx = 40,
                int forcedPxPerChar = -1,
                int panelW = 380, int panelH = 140,
                unsigned closeButton = PSP_CTRL_CROSS);

        void render(intraFont* font);
        bool update();
        void setAnimation(const MBAnimFrame* frames, size_t count, int targetH);
        void setOkStyle(float scale, unsigned color);
        void setOkAlignLeft(bool left);
        void setOkPosition(int leftPadPx, int bottomPadPx);
        void setOkTextOffset(int dx, int dy);
        void setSubtitleStyle(float scale, unsigned color);
        void setSubtitleGapAdjust(int px);
        void setInlineIcon(Texture* icon, const char* token);
        void setCancel(Texture* icon, const char* label, unsigned button = PSP_CTRL_CIRCLE);

        bool isVisible() const { return _visible; }
        void forceClose() { _visible = false; }
        bool wasCanceled() const { return _canceled; }

        // ---- Progress API (unchanged) ----
        void showProgress(const char* fileMessage, uint64_t offset, uint64_t size);
        void updateProgress(uint64_t offset, uint64_t size, const char* fileMessage = nullptr);
        void hideProgress();
        void setProgressTitle(const char* title);
        void setProgressDetailVisible(bool visible);
        void setMessage(const char* message);

private:
    // config/state
    const char* _msg;       // Title text (e.g., "Moving..." / "Copying...")
    const char* _okLabel;
    Texture*    _icon;

    int   _screenW, _screenH;
    int   _x, _y, _w, _h;   // panel rect
    int   _padX, _padY;
    int   _wrapTweakPx;
    int   _forcedPxPerChar;
    float _textScale;
    int   _iconTargetH;
    bool  _visible;

    // ---- Progress state ----
    // Two lines shown above the bar:
    //   1) _progTitle  (game title, e.g., extracted from EBOOT/ISO)
    //   2) _progDetail (filename currently being processed)
    bool        _progEnabled = false;
    std::string _progTitle;    // headline (game title)
    std::string _progDetail;   // detail (filename)
    bool        _progDetailVisible = true;
    uint64_t    _progOffset = 0;
    uint64_t    _progSize   = 1; // never 0 to avoid div-by-zero
    unsigned _closeButton;      // NEW: which button dismisses the box
    float _okScale = -1.0f;
    unsigned _okColor = 0xFFFFFFFF;
    bool _okAlignLeft = false;
    int _okLeftPad = -1;
    int _okBottomPad = 14;
    int _okTextOffsetX = 0;
    int _okTextOffsetY = 0;
    Texture* _cancelIcon = nullptr;
    const char* _cancelLabel = nullptr;
    unsigned _cancelButton = 0;
    bool _canceled = false;
    unsigned _lastButtons = 0;
    unsigned long long _holdStartUs = 0;
    unsigned _holdMask = 0;
    bool _armedForInput = true;
    bool _inputPrimed = false;

    float _subtitleScale = -1.0f;
    unsigned _subtitleColor = 0xFFFFFFFF;
    bool _useSubtitleStyle = false;
    int _subtitleGapAdjust = 0;
    Texture* _inlineIcon = nullptr;
    const char* _inlineToken = nullptr;

    // ---- Animation state ----
    const MBAnimFrame* _animFrames = nullptr;
    size_t _animCount = 0;
    size_t _animIndex = 0;
    unsigned long long _animNextUs = 0;
    int _animTargetH = 0;


};

#endif // MESSAGEBOX_H
