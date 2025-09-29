// iso_titles_extras.cpp
// Utilities for reading titles and ICON0.PNG from ISO-like containers.
// Supported:
//   - ISO
//   - CSO v1 / ZSO (global method) and CSO v2 (per-block method, per maxcso docs)
//   - JSO (LZO / zlib; robust probing)
//   - DAX (8K deflate frames)
//
// PSP/PSPSDK-friendly (sceIo*, SceUID, etc.)

#include <pspiofilemgr.h>
#include <string>
#include <vector>
#include <string.h>
#include <stdint.h>
#include <zlib.h>
#include <strings.h>

extern "C" {
#include "minilzo.h"
}

#include "lz4.h"

#ifndef ISO_SECTOR
#define ISO_SECTOR 2048
#endif

extern "C" int cmfe_titles_extras_present() { return 0x4A534F44; } // 'JSOD'

// ================================================================
// Small IO helpers
// ================================================================
static bool readAll(SceUID fd, void* buf, size_t n) {
    uint8_t* p = (uint8_t*)buf;
    size_t got = 0;
    while (got < n) {
        int r = sceIoRead(fd, p + got, (uint32_t)(n - got));
        if (r <= 0) return false;
        got += r;
    }
    return true;
}
static bool readAt(SceUID fd, uint32_t off, void* buf, size_t n) {
    if (sceIoLseek32(fd, (int)off, PSP_SEEK_SET) < 0) return false;
    return readAll(fd, buf, n);
}
static uint32_t fileSize32(SceUID fd) {
    int cur = sceIoLseek32(fd, 0, PSP_SEEK_CUR);
    int end = sceIoLseek32(fd, 0, PSP_SEEK_END);
    sceIoLseek32(fd, cur, PSP_SEEK_SET);
    return end < 0 ? 0u : (uint32_t)end;
}
static inline uint32_t le32(const uint8_t* q){
    return (uint32_t)q[0] | ((uint32_t)q[1]<<8) | ((uint32_t)q[2]<<16) | ((uint32_t)q[3]<<24);
}

// ================================================================
// SFO helpers (titles)
// ================================================================
#pragma pack(push,1)
struct SFOHeader {
    uint32_t magic;            // 'PSF\0' = 0x46535000 LE
    uint32_t version;          // 0x00000101
    uint32_t keyTableOffset;   // from start
    uint32_t dataTableOffset;  // from start
    uint32_t indexCount;
};
struct SFOIndex {
    uint16_t keyOffset;        // from key table start
    uint8_t  dataFmt;
    uint8_t  pad;
    uint32_t dataLen;
    uint32_t dataMaxLen;
    uint32_t dataOffset;       // from data table start
};
#pragma pack(pop)

static bool sfoExtractTitle(const uint8_t* data, size_t size, std::string& outTitle) {
    if (!data || size < sizeof(SFOHeader)) return false;
    const SFOHeader* h = (const SFOHeader*)data;
    if (h->magic != 0x46535000) return false; // 'PSF\0'
    if (sizeof(SFOHeader) + h->indexCount * sizeof(SFOIndex) > size) return false;

    const SFOIndex* idx = (const SFOIndex*)(data + sizeof(SFOHeader));
    const char* keys = (const char*)(data + h->keyTableOffset);
    const uint8_t* vals = data + h->dataTableOffset;

    for (uint32_t i=0;i<h->indexCount;i++) {
        const char* key = keys + idx[i].keyOffset;
        if (key && strcmp(key, "TITLE") == 0) {
            const uint8_t* v = vals + idx[i].dataOffset;
            uint32_t len = idx[i].dataLen;
            if ((size_t)(v - data) + len <= size) {
                std::string s((const char*)v, (const char*)v + len);
                while (!s.empty() && (s.back() == '\0' || s.back() == ' ')) s.pop_back();
                outTitle = s;
                return !outTitle.empty();
            }
        }
    }
    return false;
}

// ================================================================
// ISO-9660 helpers used once we can read sectors
// ================================================================
#pragma pack(push,1)
struct IsoDirRec { uint32_t lba; uint32_t size; uint8_t flags; };
#pragma pack(pop)

static bool isoReadDirRec(const uint8_t* p, size_t n, size_t off,
                          IsoDirRec& out, std::string& name, bool& isDir)
{
    if (off + 1 > n) return false;
    uint8_t len = p[off + 0];
    if (len == 0) return false;
    if (off + len > n) return false;
    const uint8_t* r = p + off;
    uint32_t lba = le32(r + 2);
    uint32_t size = le32(r + 10);
    uint8_t flags = r[25];
    uint8_t nameLen = r[32];
    const char* nm = (const char*)(r + 33);
    name.assign(nm, nm + nameLen);
    if (nameLen == 1 && (nm[0] == 0 || nm[0] == 1)) name = "";
    size_t sc = name.find(';'); if (sc != std::string::npos) name.resize(sc);
    isDir = (flags & 0x02) != 0;
    out.lba = lba; out.size = size; out.flags = flags;
    return true;
}

template<typename ReadSectorsFn>
static bool isoFindEntry(ReadSectorsFn readSectors,
                         void* ctx,
                         const IsoDirRec& dir,
                         const char* target,
                         IsoDirRec& out)
{
    uint32_t bytes = ((dir.size + ISO_SECTOR - 1)/ISO_SECTOR)*ISO_SECTOR;
    std::vector<uint8_t> buf(bytes);
    if (!readSectors(ctx, dir.lba, bytes/ISO_SECTOR, buf.data())) return false;

    size_t pos = 0;
    while (pos < bytes) {
        if (buf[pos] == 0) { pos = ((pos/ISO_SECTOR)+1)*ISO_SECTOR; continue; }
        IsoDirRec r{}; std::string nm; bool isDir=false;
        if (!isoReadDirRec(buf.data(), bytes, pos, r, nm, isDir)) break;
        if (!nm.empty() && strcasecmp(nm.c_str(), target) == 0) { out = r; return true; }
        pos += buf[pos];
    }
    return false;
}

template<typename ReadSectorsFn>
static bool readTitleViaSectors(ReadSectorsFn readSectors, void* ctx, std::string& outTitle)
{
    // PVD @ sector 16
    std::vector<uint8_t> pvd(ISO_SECTOR);
    if (!readSectors(ctx, 16, 1, pvd.data())) return false;
    if (!(pvd[0]==1 && memcmp(&pvd[1],"CD001",5)==0 && pvd[6]==1)) return false;

    // root dir @156
    IsoDirRec root{}; { std::string nm; bool isDir=false;
        if (!isoReadDirRec(pvd.data(), ISO_SECTOR, 156, root, nm, isDir)) return false; }

    // PSP_GAME/PARAM.SFO
    IsoDirRec pspGame{}; if (!isoFindEntry(readSectors, ctx, root, "PSP_GAME", pspGame)) return false;
    IsoDirRec param{};   if (!isoFindEntry(readSectors, ctx, pspGame, "PARAM.SFO", param)) return false;
    if (param.size == 0 || param.size > 512*1024) return false;

    uint32_t need = ((param.size + ISO_SECTOR - 1)/ISO_SECTOR)*ISO_SECTOR;
    std::vector<uint8_t> sfo(need);
    if (!readSectors(ctx, param.lba, need/ISO_SECTOR, sfo.data())) return false;

    return sfoExtractTitle(sfo.data(), param.size, outTitle);
}

template<typename ReadSectorsFn>
static bool readIconViaSectors(ReadSectorsFn readSectors, void* ctx, std::vector<uint8_t>& outVec)
{
    outVec.clear();

    // PVD @16
    std::vector<uint8_t> pvd(ISO_SECTOR);
    if (!readSectors(ctx, 16, 1, pvd.data())) return false;
    if (!(pvd[0]==1 && memcmp(&pvd[1],"CD001",5)==0 && pvd[6]==1)) return false;

    // root dir @156
    IsoDirRec root{}; { std::string nm; bool isDir=false;
        if (!isoReadDirRec(pvd.data(), ISO_SECTOR, 156, root, nm, isDir)) return false; }

    IsoDirRec pspGame{}; if (!isoFindEntry(readSectors, ctx, root, "PSP_GAME", pspGame)) return false;
    IsoDirRec icon{};    if (!isoFindEntry(readSectors, ctx, pspGame, "ICON0.PNG", icon)) return false;
    if (!icon.size || icon.size > 1024*1024) return false;

    uint32_t need = ((icon.size + ISO_SECTOR - 1)/ISO_SECTOR)*ISO_SECTOR;
    std::vector<uint8_t> tmp(need);
    if (!readSectors(ctx, icon.lba, need/ISO_SECTOR, tmp.data())) return false;

    outVec.assign(tmp.begin(), tmp.begin() + icon.size);
    return true;
}

// ================================================================
// Plain ISO title
// ================================================================
bool readIsoTitle(const std::string& isoPath, std::string& outTitle) {
    SceUID fd = sceIoOpen(isoPath.c_str(), PSP_O_RDONLY, 0);
    if (fd < 0) return false;
    auto readSec = [](void* vfd, uint32_t lba, uint32_t cnt, uint8_t* out)->bool{
        return readAt((SceUID)(intptr_t)vfd, lba * ISO_SECTOR, out, cnt * ISO_SECTOR);
    };
    bool ok = readTitleViaSectors(readSec, (void*)(intptr_t)fd, outTitle);
    sceIoClose(fd);
    return ok;
}

// ================================================================
// CSO/ZSO reader — includes **CSO v2 per-block method** support
// and a proper block cache for 2K/4K/16K blocks.
// ================================================================
#pragma pack(push,1)
struct CISOHeader {
    uint32_t magic;        // 'CISO' or 'ZISO'
    uint32_t header_size;  // offset to index table (v2 requires 0x18)
    uint64_t total_bytes;  // uncompressed size
    uint32_t block_size;   // usually 2048 or 16384
    uint8_t  version;      // 0/1, or 2 (experimental v2)
    uint8_t  align;        // index_shift (left shift for offsets)
    uint8_t  reserved[2];
};
#pragma pack(pop)

struct CompressedIso {
    SceUID   fd = -1;
    bool     isZSO = false;       // ZISO = LZ4 for compressed blocks (v1 semantics)
    uint8_t  version = 0;         // 0/1 = v1, 2 = v2
    bool     isCisoV2 = false;    // true when magic 'CISO' and version==2
    uint32_t block_size = ISO_SECTOR;
    uint8_t  align = 0;
    uint32_t index_off = 0;
    uint32_t file_size = 0;       // for end-guard

    // block cache
    int32_t  cached_block = -1;
    std::vector<uint8_t> blockBuf;
};

static bool inflateRawOrZlib(const uint8_t* in, uint32_t inLen, uint8_t* out, uint32_t outLen){
    // Try raw DEFLATE first
    {
        z_stream zs; memset(&zs, 0, sizeof(zs));
        zs.next_in = (Bytef*)in;  zs.avail_in  = inLen;
        zs.next_out= out;         zs.avail_out = outLen;
        if (inflateInit2(&zs, -MAX_WBITS) == Z_OK) {
            int ret = inflate(&zs, Z_FINISH);
            bool ok = (ret == Z_STREAM_END) && (zs.total_out == outLen);
            inflateEnd(&zs);
            if (ok) return true;
        }
    }
    // Fallback: zlib-wrapped
    {
        z_stream zs; memset(&zs, 0, sizeof(zs));
        zs.next_in = (Bytef*)in;  zs.avail_in  = inLen;
        zs.next_out= out;         zs.avail_out = outLen;
        if (inflateInit(&zs) == Z_OK) {
            int ret = inflate(&zs, Z_FINISH);
            bool ok = (ret == Z_STREAM_END) && (zs.total_out == outLen);
            inflateEnd(&zs);
            if (ok) return true;
        }
    }
    return false;
}

static bool cisoOpen(const std::string& path, CompressedIso& out) {
    out.fd = sceIoOpen(path.c_str(), PSP_O_RDONLY, 0);
    if (out.fd < 0) return false;

    CISOHeader h{};
    if (!readAt(out.fd, 0, &h, sizeof(h))) { sceIoClose(out.fd); return false; }

    if (h.magic == 0x4F534943 /* 'CISO' */) { out.isZSO = false; out.version = h.version; out.isCisoV2 = (h.version == 2); }
    else if (h.magic == 0x4F53495A /* 'ZISO' */) { out.isZSO = true; out.version = h.version; out.isCisoV2 = false; }
    else { sceIoClose(out.fd); return false; }

    if (h.block_size == 0) { sceIoClose(out.fd); return false; }
    out.block_size = h.block_size;
    out.align      = h.align;
    out.index_off  = h.header_size ? (uint32_t)h.header_size : (uint32_t)sizeof(h);

    // Record file size (for end-guard)
    SceIoStat st{};
    if (sceIoGetstat(path.c_str(), &st) >= 0) out.file_size = (uint32_t)st.st_size;

    // Prepare cache buffer
    out.blockBuf.resize(out.block_size);
    out.cached_block = -1;

    return true;
}

static void cisoClose(CompressedIso& ci) {
    if (ci.fd >= 0) sceIoClose(ci.fd);
    ci.fd = -1;
    ci.blockBuf.clear();
    ci.cached_block = -1;
}

// Decompress/populate a full block into ci.blockBuf (size = block_size).
static bool cisoFillBlock(CompressedIso& ci, uint32_t blkIdx) {
    // Each block corresponds to block_size bytes of uncompressed data.
    // Index table is per *block*, not per 2048 sector.
    uint32_t i0 = 0, i1 = 0;
    uint32_t idxOff = ci.index_off + blkIdx * 4;
    if (!readAt(ci.fd, idxOff,     &i0, 4)) return false;
    if (!readAt(ci.fd, idxOff + 4, &i1, 4)) {
        // Last index missing: use file size as end pointer.
        if (!ci.file_size) return false;
        i1 = ((ci.file_size >> ci.align) & 0x7FFFFFFF);
    }

    uint32_t off0 = (i0 & 0x7FFFFFFF) << ci.align;
    uint32_t off1 = (i1 & 0x7FFFFFFF) << ci.align;
    if (ci.file_size && (off1 <= off0 || off1 > ci.file_size)) off1 = ci.file_size;

    uint32_t compSize = (off1 > off0) ? (off1 - off0) : 0;

    if (ci.isCisoV2) {
        // v2 rule: size >= block_size ⇒ stored, regardless of MSB
        bool stored = (compSize >= ci.block_size);
        if (stored) {
            if (compSize < ci.block_size) return false;
            return readAt(ci.fd, off0, ci.blockBuf.data(), ci.block_size);
        }
        // compressed: MSB set ⇒ LZ4, clear ⇒ deflate
        bool methodLZ4 = (i0 & 0x80000000u) != 0;
        if (compSize == 0 || compSize > 1024*1024) return false;
        std::vector<uint8_t> in(compSize);
        if (!readAt(ci.fd, off0, in.data(), compSize)) return false;

        if (methodLZ4) {
            int r = LZ4_decompress_safe((const char*)in.data(), (char*)ci.blockBuf.data(),
                                        (int)compSize, (int)ci.block_size);
            return r == (int)ci.block_size;
        } else {
            return inflateRawOrZlib(in.data(), compSize, ci.blockBuf.data(), ci.block_size);
        }
    } else {
        // v1/ZSO semantics: MSB = stored; compressed method is global (ZSO=LZ4, CISO=deflate)
        bool stored = (i0 & 0x80000000u) != 0;

        if (stored || compSize == ci.block_size) {
            if (compSize < ci.block_size) return false;
            return readAt(ci.fd, off0, ci.blockBuf.data(), ci.block_size);
        }

        if (compSize == 0 || compSize > 1024*1024) return false;

        std::vector<uint8_t> in(compSize);
        if (!readAt(ci.fd, off0, in.data(), compSize)) return false;

        if (ci.isZSO) {
            int r = LZ4_decompress_safe((const char*)in.data(), (char*)ci.blockBuf.data(),
                                        (int)compSize, (int)ci.block_size);
            return r == (int)ci.block_size;
        } else {
            return inflateRawOrZlib(in.data(), compSize, ci.blockBuf.data(), ci.block_size);
        }
    }
}

// Sector API that serves 2048-byte slices from the cached block.
static bool cisoReadSectors(CompressedIso& ci, uint32_t lba, uint32_t count, uint8_t* buf) {
    if (ci.block_size < ISO_SECTOR || (ci.block_size % ISO_SECTOR) != 0) return false;
    const uint32_t spb = ci.block_size / ISO_SECTOR;  // sectors per block

    for (uint32_t i = 0; i < count; ++i) {
        uint32_t L      = lba + i;
        uint32_t blkIdx = L / spb;
        uint32_t sub    = L % spb;

        if ((int32_t)blkIdx != ci.cached_block) {
            if (!cisoFillBlock(ci, blkIdx)) return false;
            ci.cached_block = (int32_t)blkIdx;
        }
        memcpy(buf + i * ISO_SECTOR, ci.blockBuf.data() + sub * ISO_SECTOR, ISO_SECTOR);
    }
    return true;
}

bool readCompressedIsoTitle(const std::string& path, std::string& outTitle) {
    CompressedIso ci;
    if (!cisoOpen(path, ci)) return false;

    auto readSec = [](void* vci, uint32_t l, uint32_t c, uint8_t* o)->bool{
        return cisoReadSectors(*(CompressedIso*)vci, l, c, o);
    };
    bool ok = readTitleViaSectors(readSec, &ci, outTitle);
    cisoClose(ci);
    return ok;
}

// ================================================================
// JSO reader — robust "probe" opener for multiple variants
// ================================================================
struct JsoCtx {
    SceUID   fd;
    uint32_t index_off;
    uint32_t block_size;
    uint8_t  align;   // shift for offsets (0..4)
    uint8_t  method;  // 1=zlib, 2=lzo
    uint32_t file_size;
};

static bool jsoDecompress(const uint8_t* in, uint32_t inLen, uint8_t* out, uint32_t outLen, uint8_t method){
    if (method == 2) { // LZO
        lzo_uint out_len = outLen;
        int r = lzo1x_decompress_safe(in, inLen, out, &out_len, NULL);
        if (r == LZO_E_OK && out_len == outLen) return true;
        // fall through to try zlib if header lied
    }
    // method == 1 (zlib) or fallback
    return inflateRawOrZlib(in, inLen, out, outLen);
}

static bool jsoReadBlock(JsoCtx* ctx, uint32_t blkIdx, uint8_t* out) {
    uint32_t i0=0, i1=0;
    if (!readAt(ctx->fd, ctx->index_off + blkIdx*4,     &i0, 4)) return false;
    if (!readAt(ctx->fd, ctx->index_off + (blkIdx+1)*4, &i1, 4)) return false;

    bool stored = (i0 & 0x80000000u) != 0;
    uint32_t off0 = (i0 & 0x7FFFFFFFu) << ctx->align;
    uint32_t off1 = (i1 & 0x7FFFFFFFu) << ctx->align;
    if (off1 <= off0 || off1 > ctx->file_size) off1 = ctx->file_size;

    uint32_t compSize = (off1 > off0) ? (off1 - off0) : 0;

    if (stored) {
        if (compSize < ctx->block_size) return false;
        return readAt(ctx->fd, off0, out, ctx->block_size);
    }

    if (compSize == 0 || compSize > 1024*1024) return false;

    std::vector<uint8_t> in(compSize);
    if (!readAt(ctx->fd, off0, in.data(), compSize)) return false;

    if (jsoDecompress(in.data(), compSize, out, ctx->block_size, ctx->method))
        return true;

    // Some JSO writers fail to mark stored, but compSize == block_size → treat as raw
    if (compSize == ctx->block_size)
        return readAt(ctx->fd, off0, out, ctx->block_size);

    return false;
}

static bool jsoReadSectors(void* vctx, uint32_t lba, uint32_t count, uint8_t* out) {
    JsoCtx* ctx = (JsoCtx*)vctx;
    if (ctx->block_size < ISO_SECTOR || (ctx->block_size % ISO_SECTOR) != 0) return false;

    const uint32_t spb = ctx->block_size / ISO_SECTOR;  // sectors per compressed block
    std::vector<uint8_t> block(ctx->block_size);

    for (uint32_t i = 0; i < count; ++i) {
        uint32_t L      = lba + i;
        uint32_t blkIdx = L / spb;
        uint32_t sub    = L % spb;

        if (!jsoReadBlock(ctx, blkIdx, block.data())) return false;
        memcpy(out + i * ISO_SECTOR, block.data() + sub * ISO_SECTOR, ISO_SECTOR);
    }
    return true;
}

// Probe for index offset by using a simple structural heuristic.
static bool jsoFindIndexOffset(const uint8_t* hdr, uint32_t hdrSize, uint32_t fsize, uint32_t& index_off_out) {
    uint32_t limit = (hdrSize < 0x800) ? hdrSize : 0x800;
    for (uint32_t s = 0x10; s + 8 <= limit; s += 4) {
        uint32_t a = le32(hdr + s);
        if (a > s + 8 && a < fsize && ((a - s) % 4) == 0) {
            uint32_t blocks_plus1 = (a - s) / 4;
            if (blocks_plus1 > 16 && blocks_plus1 < 4u*1024u*1024u) {
                index_off_out = s;
                return true;
            }
        }
    }
    return false;
}

static bool jsoOpen(SceUID fd, JsoCtx*& outCtx) {
    outCtx = nullptr;
    (void)lzo_init(); // safe to call multiple times

    uint8_t hdr[0x400] = {0};
    if (!readAt(fd, 0, hdr, sizeof(hdr))) return false;
    if (memcmp(hdr, "JISO", 4) != 0) return false;

    uint32_t fsize = fileSize32(fd);
    if (!fsize) return false;

    uint32_t index_off = 0;
    if (!jsoFindIndexOffset(hdr, sizeof(hdr), fsize, index_off)) index_off = 0x20;

    const uint32_t blockCands[] = { 2048, 4096, 8192 };
    const uint8_t  alignCands[] = { 0, 1, 2, 3, 4 };
    const uint8_t  methodCands[] = { 2 /*LZO*/, 1 /*zlib*/ };

    for (uint32_t bs : blockCands) {
        for (uint8_t al : alignCands) {
            for (uint8_t m : methodCands) {
                JsoCtx* ctx = new JsoCtx();
                ctx->fd         = fd;
                ctx->index_off  = index_off;
                ctx->block_size = bs;
                ctx->align      = al;
                ctx->method     = m;
                ctx->file_size  = fsize;

                std::vector<uint8_t> pvd(ISO_SECTOR);
                if (jsoReadSectors(ctx, 16, 1, pvd.data()) &&
                    pvd[0]==1 && memcmp(&pvd[1],"CD001",5)==0 && pvd[6]==1) {
                    outCtx = ctx;
                    return true;
                }
                delete ctx;
            }
        }
    }
    return false;
}
static void jsoClose(JsoCtx*& ctx) { if (ctx) { delete ctx; ctx=nullptr; } }

bool readJsoTitle(const std::string& path, std::string& outTitle) {
    SceUID fd = sceIoOpen(path.c_str(), PSP_O_RDONLY, 0);
    if (fd < 0) return false;
    JsoCtx* ctx = nullptr;
    bool ok = jsoOpen(fd, ctx) && readTitleViaSectors(jsoReadSectors, ctx, outTitle);
    if (ctx) jsoClose(ctx);
    sceIoClose(fd);
    return ok;
}
bool readJsoIconPNG(const std::string& path, std::vector<uint8_t>& outVec) {
    SceUID fd = sceIoOpen(path.c_str(), PSP_O_RDONLY, 0);
    if (fd < 0) return false;
    JsoCtx* ctx = nullptr;
    bool ok = jsoOpen(fd, ctx) && readIconViaSectors(jsoReadSectors, ctx, outVec);
    if (ctx) jsoClose(ctx);
    sceIoClose(fd);
    return ok;
}

// ================================================================
// DAX reader — pragmatic variant detector (8K deflate frames)
// ================================================================
struct DaxCtx {
    SceUID   fd;
    uint32_t index_off;
    uint32_t block_size; // 8K typical
    uint8_t  align;      // shift for offsets (often 0..4)
    bool     msbStored;  // whether high bit of index marks "stored"
};

static bool daxDecompress(const uint8_t* in, uint32_t inLen, uint8_t* out, uint32_t outLen){
    return inflateRawOrZlib(in, inLen, out, outLen);
}

static bool daxReadBlock(DaxCtx* ctx, uint32_t lba, uint8_t* out) {
    uint32_t sectorsPerFrame = ctx->block_size / ISO_SECTOR; // usually 4
    uint32_t frameIndex = lba / sectorsPerFrame;
    uint32_t sub        = lba % sectorsPerFrame;

    uint32_t idxOff = ctx->index_off + frameIndex*4;
    uint32_t i0=0, i1=0;
    if (!readAt(ctx->fd, idxOff,   &i0, 4)) return false;
    if (!readAt(ctx->fd, idxOff+4, &i1, 4)) return false;

    uint32_t off0 = (i0 & 0x7FFFFFFF) << ctx->align;
    uint32_t off1 = (i1 & 0x7FFFFFFF) << ctx->align;
    uint32_t compSize = (off1 > off0) ? (off1 - off0) : 0;
    bool stored = ctx->msbStored ? ((i0 & 0x80000000u) != 0) : false;

    if (compSize == 0 || compSize > 1*1024*1024) return false;

    std::vector<uint8_t> frameBuf(ctx->block_size);
    if (stored) {
        if (!readAt(ctx->fd, off0, frameBuf.data(), ctx->block_size)) return false;
    } else {
        std::vector<uint8_t> in(compSize);
        if (!readAt(ctx->fd, off0, in.data(), compSize)) return false;
        if (!daxDecompress(in.data(), compSize, frameBuf.data(), ctx->block_size)) return false;
    }

    memcpy(out, frameBuf.data() + sub*ISO_SECTOR, ISO_SECTOR);
    return true;
}

static bool daxReadSectors(void* vctx, uint32_t lba, uint32_t count, uint8_t* out) {
    DaxCtx* ctx = (DaxCtx*)vctx;
    for (uint32_t i = 0; i < count; ++i)
        if (!daxReadBlock(ctx, lba + i, out + i*ISO_SECTOR)) return false;
    return true;
}

static bool daxTryProbe(SceUID fd, uint32_t headerSize, uint32_t blockSize, uint8_t align, bool msbStored, DaxCtx*& out) {
    DaxCtx* ctx = new DaxCtx();
    ctx->fd = fd; ctx->index_off = headerSize; ctx->block_size = blockSize;
    ctx->align = align; ctx->msbStored = msbStored;

    std::vector<uint8_t> pvd(ISO_SECTOR);
    bool ok = daxReadSectors(ctx, 16, 1, pvd.data())
              && pvd[0]==1 && memcmp(&pvd[1],"CD001",5)==0 && pvd[6]==1;
    if (!ok) { delete ctx; return false; }
    out = ctx; return true;
}

static bool daxOpen(SceUID fd, DaxCtx*& outCtx) {
    outCtx = nullptr;

    uint8_t hdr[64]; if (!readAt(fd, 0, hdr, sizeof(hdr))) return false;
    bool magicDAX = (memcmp(hdr, "DAX", 3) == 0);
    bool magicDAX0 = (memcmp(hdr, "DAX\0", 4) == 0) || (memcmp(hdr, "DAX ", 4) == 0);
    const uint32_t DAX_FRAME = 8*1024;

    struct Candidate { uint32_t hsz; uint8_t align; bool msb; } cands[] = {
        { 0x20, 0, true }, { 0x20, 1, true }, { 0x20, 2, true },
        { 0x18, 0, true }, { 0x18, 1, true }, { 0x18, 2, true },
        { 0x24, 0, true }, { 0x24, 1, true }, { 0x24, 2, true },
        { 0x20, 0, false}, { 0x20, 1, false}, { 0x20, 2, false}
    };

    if (!magicDAX && !magicDAX0) return false;

    for (auto &c : cands) {
        DaxCtx* ctx = nullptr;
        if (daxTryProbe(fd, c.hsz, DAX_FRAME, c.align, c.msb, ctx)) {
            outCtx = ctx;
            return true;
        }
    }
    return false;
}
static void daxClose(DaxCtx*& ctx){ if (ctx){ delete ctx; ctx=nullptr; } }

bool readDaxTitle(const std::string& path, std::string& outTitle) {
    SceUID fd = sceIoOpen(path.c_str(), PSP_O_RDONLY, 0);
    if (fd < 0) return false;
    DaxCtx* ctx = nullptr;
    bool ok = daxOpen(fd, ctx) && readTitleViaSectors(daxReadSectors, ctx, outTitle);
    if (ctx) daxClose(ctx);
    sceIoClose(fd);
    return ok;
}
bool readDaxIconPNG(const std::string& path, std::vector<uint8_t>& outVec) {
    SceUID fd = sceIoOpen(path.c_str(), PSP_O_RDONLY, 0);
    if (fd < 0) return false;
    DaxCtx* ctx = nullptr;
    bool ok = daxOpen(fd, ctx) && readIconViaSectors(daxReadSectors, ctx, outVec);
    if (ctx) daxClose(ctx);
    sceIoClose(fd);
    return ok;
}

// ================================================================
// Public convenience: pick the right extractor for ICON0
// ================================================================
static char toLowerC(char c){ return (c>='A'&&c<='Z')? (char)(c-'A'+'a'):c; }
static bool endsWithNoCase(const std::string& s, const char* ext){
    size_t n = s.size(), m = strlen(ext);
    if (m>n) return false;
    for (size_t i=0;i<m;i++) if (toLowerC(s[n-m+i]) != toLowerC(ext[i])) return false;
    return true;
}

// --- PNG signature fallback extractor ---------------------------------------
// Scan the file for a PNG signature and copy bytes chunk-by-chunk until IEND.
// This is used only if ISO9660 lookup fails (handles weird padding layouts).
static bool ExtractPNGBySignature(const std::string& path, std::vector<uint8_t>& out) {
    // PNG signature
    static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};

    SceUID fd = sceIoOpen(path.c_str(), PSP_O_RDONLY, 0);
    if (fd < 0) return false;

    // Stream state
    const int READ_SZ = 256 * 1024;
    std::vector<uint8_t> buf(READ_SZ + 8);
    int64_t filePos = 0;
    int carry = 0;
    bool found = false;

    // Rolling scan for signature
    while (true) {
        int got = sceIoRead(fd, buf.data() + carry, READ_SZ);
        if (got < 0) { sceIoClose(fd); return false; }
        if (got == 0 && carry == 0) { break; }
        int total = carry + got;

        for (int i = 0; i + 8 <= total; ++i) {
            if (memcmp(buf.data() + i, sig, 8) == 0) {
                // Found a PNG. Seek to absolute position of signature.
                int64_t sigAbs = filePos + i;
                sceIoLseek32(fd, (SceOff)sigAbs, PSP_SEEK_SET);

                // Copy the PNG out by parsing length+type+data+CRC until IEND.
                out.clear();
                out.reserve(256 * 1024); // typical ICON0 size
                // Write signature
                out.insert(out.end(), sig, sig + 8);

                // Read chunks
                while (true) {
                    uint8_t lenType[8];
                    int r = sceIoRead(fd, lenType, 8);
                    if (r != 8) { out.clear(); sceIoClose(fd); return false; }

                    uint32_t len = (lenType[0] << 24) | (lenType[1] << 16) | (lenType[2] << 8) | lenType[3];
                    // Guard: ICON0.PNG is small; cap at ~5MB to avoid bogus scans.
                    if (len > 5 * 1024 * 1024) { out.clear(); sceIoClose(fd); return false; }

                    // Append length+type
                    out.insert(out.end(), lenType, lenType + 8);

                    // Read data + CRC
                    size_t chunkTotal = (size_t)len + 4; // CRC
                    size_t already = out.size();
                    out.resize(already + chunkTotal);
                    if (sceIoRead(fd, out.data() + already, chunkTotal) != (int)chunkTotal) {
                        out.clear(); sceIoClose(fd); return false;
                    }

                    // Check for IEND
                    if (lenType[4] == 'I' && lenType[5] == 'E' && lenType[6] == 'N' && lenType[7] == 'D') {
                        sceIoClose(fd);
                        return true;
                    }
                }
            }
        }

        // Keep last 7 bytes to detect signature across chunk boundary
        carry = std::min(total, 7);
        if (carry) memmove(buf.data(), buf.data() + total - carry, carry);
        filePos += got;
        if (got == 0) break;
    }

    sceIoClose(fd);
    return false;
}


bool ExtractIcon0PNG(const std::string& path, std::vector<uint8_t>& outVec) {
    outVec.clear();

    if (endsWithNoCase(path, ".iso")) {
        SceUID fd = sceIoOpen(path.c_str(), PSP_O_RDONLY, 0);
        if (fd < 0) return false;
        auto readSec = [](void* vfd, uint32_t lba, uint32_t cnt, uint8_t* out)->bool{
            return readAt((SceUID)(intptr_t)vfd, lba * ISO_SECTOR, out, cnt * ISO_SECTOR);
        };
        bool ok = readIconViaSectors(readSec, (void*)(intptr_t)fd, outVec);
        sceIoClose(fd); return ok;
    }

    if (endsWithNoCase(path, ".cso") || endsWithNoCase(path, ".zso")) {
        CompressedIso ci; if (!cisoOpen(path, ci)) return false;
        auto readSec = [](void* vci, uint32_t lba, uint32_t cnt, uint8_t* out)->bool{
            return cisoReadSectors(*(CompressedIso*)vci, lba, cnt, out);
        };
        bool ok = readIconViaSectors(readSec, &ci, outVec);
        cisoClose(ci); return ok;
    }

    if (endsWithNoCase(path, ".dax")) {
        return readDaxIconPNG(path, outVec);
    }

    if (endsWithNoCase(path, ".jso")) {
        return readJsoIconPNG(path, outVec);
    }

    {
        std::vector<uint8_t> fallback;
        if (ExtractPNGBySignature(path, fallback)) {
            outVec.swap(fallback);
            // Optional log: printf("[icon] Fallback PNG signature scan succeeded on %s\n", path.c_str());
            return true;
        }
    }

    // Still no icon
    return false;
}
