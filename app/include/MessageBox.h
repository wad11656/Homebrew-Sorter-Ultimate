// include/MessageBox.h
#ifndef MESSAGEBOX_H
#define MESSAGEBOX_H

#include <pspctrl.h>
#include <intraFont.h>
#include <string>
#include "Texture.h"

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
                unsigned closeButton = PSP_CTRL_CROSS);

        void render(intraFont* font);
        bool update();

        bool isVisible() const { return _visible; }
        void forceClose() { _visible = false; }

        // ---- Progress API (unchanged) ----
        void showProgress(const char* fileMessage, uint64_t offset, uint64_t size);
        void updateProgress(uint64_t offset, uint64_t size, const char* fileMessage = nullptr);
        void hideProgress();
        void setProgressTitle(const char* title);

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
    uint64_t    _progOffset = 0;
    uint64_t    _progSize   = 1; // never 0 to avoid div-by-zero
    unsigned _closeButton;      // NEW: which button dismisses the box


};

#endif // MESSAGEBOX_H
