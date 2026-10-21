#include "Texture.h"
#include <pspgu.h>
#include <pspiofilemgr.h>
#include <string.h>
#include <malloc.h>

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>   // you already have this in ../libs/include

// Reads the whole file in one sceIoRead and decodes from memory. stbi_load()
// would go through stdio, which refills a small buffer over and over -- dozens
// of Memory Stick reads per ICON0 instead of one.
Texture* texLoadPNG(const char* path) {
    if (!path) return nullptr;
    SceUID fd = sceIoOpen(path, PSP_O_RDONLY, 0);
    if (fd < 0) return nullptr;
    SceOff size = sceIoLseek(fd, 0, PSP_SEEK_END);
    if (size <= 0 || size > 16 * 1024 * 1024) { sceIoClose(fd); return nullptr; }
    sceIoLseek(fd, 0, PSP_SEEK_SET);

    unsigned char* buf = (unsigned char*)malloc((size_t)size);
    if (!buf) { sceIoClose(fd); return nullptr; }
    int got = 0;
    while (got < (int)size) {
        int r = sceIoRead(fd, buf + got, (int)size - got);
        if (r <= 0) break;
        got += r;
    }
    sceIoClose(fd);

    Texture* t = (got == (int)size) ? texLoadPNGFromMemory(buf, got) : nullptr;
    free(buf);
    return t;
}

void texFree(Texture* t) {
    if (!t) return;
    if (t->data) free(t->data);
    free(t);
}

Texture* texLoadPNGFromMemory(const unsigned char* data, int len) {
    int w=0, h=0, comp=0;
    unsigned char* pix = stbi_load_from_memory(data, len, &w, &h, &comp, STBI_rgb_alpha);
    if (!pix) return nullptr;

    int tw = 1; while (tw < w) tw <<= 1;
    int th = 1; while (th < h) th <<= 1;

    uint32_t* p2buf = (uint32_t*)memalign(16, tw * th * 4);
    if (!p2buf) { stbi_image_free(pix); return nullptr; }
    memset(p2buf, 0, tw * th * 4);

    for (int y = 0; y < h; ++y) memcpy(p2buf + y * tw, pix + y * w * 4, w * 4);
    stbi_image_free(pix);

    Texture* t = (Texture*)malloc(sizeof(Texture));
    if (!t) { free(p2buf); return nullptr; }
    t->width  = w;
    t->height = h;
    t->stride = tw;
    t->data   = p2buf;
    return t;
}
