// -----------------------------------------------------------
// New: Move operation helpers
// -----------------------------------------------------------
namespace {
bool sKfeCriticalGuardFailure = false;

struct KfeCopyPathPair {
    std::string src;
    std::string dst;
};

static bool kfeEndsWithNoCase(const std::string& s, const char* suffix) {
    if (!suffix) return false;
    const size_t n = std::strlen(suffix);
    if (s.size() < n) return false;
    return strncasecmp(s.c_str() + s.size() - n, suffix, n) == 0;
}

static bool kfeIsMacJunkName(const char* name) {
    if (!name || !*name) return false;
    // AppleDouble sidecar files from macOS copy operations.
    if (name[0] == '.' && name[1] == '_' && name[2] != '\0') return true;
    // Finder metadata file.
    if (!strcasecmp(name, ".DS_Store")) return true;
    return false;
}

static bool kfeNeedsDestPresenceVerify(const std::string& path) {
    return kfeEndsWithNoCase(path, ".pbp") || kfeEndsWithNoCase(path, ".prx");
}

static bool kfeWaitForPathPresence(const std::string& path) {
    if (pathExists(path)) return true;
    // Some cards/CFW/plugin stacks can report file visibility with slight lag.
    sceKernelDelayThread(8 * 1000);
    return pathExists(path);
}

static void kfeCollectAllSourceFiles(const std::string& srcDir,
                                     const std::string& dstDir,
                                     std::vector<KfeCopyPathPair>& out,
                                     bool& scanOk) {
    SceUID d = kfeIoOpenDir(srcDir.c_str());
    if (d < 0) { scanOk = false; return; }

    SceIoDirent ent; memset(&ent, 0, sizeof(ent));
    while (kfeIoReadDir(d, &ent) > 0) {
        if (!strcmp(ent.d_name, ".") || !strcmp(ent.d_name, "..")) {
            memset(&ent, 0, sizeof(ent));
            continue;
        }
        if (kfeIsMacJunkName(ent.d_name)) {
            memset(&ent, 0, sizeof(ent));
            continue;
        }
        std::string s = joinDirFile(srcDir, ent.d_name);
        std::string t = joinDirFile(dstDir, ent.d_name);
        if (FIO_S_ISDIR(ent.d_stat.st_mode)) kfeCollectAllSourceFiles(s, t, out, scanOk);
        else out.push_back({s, t});
        memset(&ent, 0, sizeof(ent));
        sceKernelDelayThread(0);
    }
    kfeIoCloseDir(d);
}

static void kfeCollectMissingDestFiles(const std::string& srcDir,
                                       const std::string& dstDir,
                                       std::vector<KfeCopyPathPair>& out,
                                       bool& scanOk) {
    SceUID d = kfeIoOpenDir(srcDir.c_str());
    if (d < 0) { scanOk = false; return; }

    SceIoDirent ent; memset(&ent, 0, sizeof(ent));
    while (kfeIoReadDir(d, &ent) > 0) {
        if (!strcmp(ent.d_name, ".") || !strcmp(ent.d_name, "..")) {
            memset(&ent, 0, sizeof(ent));
            continue;
        }
        if (kfeIsMacJunkName(ent.d_name)) {
            memset(&ent, 0, sizeof(ent));
            continue;
        }

        std::string s = joinDirFile(srcDir, ent.d_name);
        std::string t = joinDirFile(dstDir, ent.d_name);
        if (FIO_S_ISDIR(ent.d_stat.st_mode)) {
            SceIoStat st{};
            if (sceIoGetstat(t.c_str(), &st) < 0 || !FIO_S_ISDIR(st.st_mode)) {
                kfeCollectAllSourceFiles(s, t, out, scanOk);
            } else {
                kfeCollectMissingDestFiles(s, t, out, scanOk);
            }
        } else {
            SceIoStat st{};
            const bool missing = (sceIoGetstat(t.c_str(), &st) < 0) || FIO_S_ISDIR(st.st_mode);
            if (missing) out.push_back({s, t});
        }
        memset(&ent, 0, sizeof(ent));
        sceKernelDelayThread(0);
    }
    kfeIoCloseDir(d);
}

// Remove every destination entry whose name matches leaf case-insensitively.
// This prevents BOOT/boot duplicate-name collisions before copy/move.
static void kfeRemoveCaseCollisionsInDir(const std::string& dir, const std::string& leaf) {
    if (dir.empty() || leaf.empty()) return;
    SceUID d = kfeIoOpenDir(dir.c_str());
    if (d < 0) return;

    std::vector<std::string> matches;
    SceIoDirent ent; memset(&ent, 0, sizeof(ent));
    while (kfeIoReadDir(d, &ent) > 0) {
        trimTrailingSpaces(ent.d_name);
        if (!strcmp(ent.d_name, ".") || !strcmp(ent.d_name, "..")) {
            memset(&ent, 0, sizeof(ent));
            continue;
        }
        if (!strcasecmp(ent.d_name, leaf.c_str())) {
            matches.push_back(joinDirFile(dir, ent.d_name));
        }
        memset(&ent, 0, sizeof(ent));
    }
    kfeIoCloseDir(d);

    for (size_t i = 0; i < matches.size(); ++i) {
        SceIoStat st{};
        if (!pathExists(matches[i], &st)) continue;
        if (isDirMode(st)) removeDirRecursive(matches[i]);
        else sceIoRemove(matches[i].c_str());
    }
}
}

void KfeFileOps::resetCriticalGuardFailure() { sKfeCriticalGuardFailure = false; }
bool KfeFileOps::hasCriticalGuardFailure() { return sKfeCriticalGuardFailure; }

// (rootPrefix removed — we already have this earlier in the class)
bool KfeFileOps::sameDevice(const std::string& a, const std::string& b) {
    return strncasecmp(a.c_str(), b.c_str(), 5) == 0;
}
bool KfeFileOps::ensureDir(const std::string& path) {
    // create single level
    if (dirExists(path)) return true;
    return sceIoMkdir(path.c_str(), 0777) >= 0;
}
bool KfeFileOps::ensureDirRecursive(const std::string& full) {
    // make every segment after "ms0:/" or "ef0:/"
    if (full.size() < 5) return false;
    size_t i = 5; // skip "ms0:/"
    while (i < full.size()) {
        size_t j = full.find('/', i);
        if (j == std::string::npos) j = full.size();
        std::string sub = full.substr(0, j);
        if (!dirExists(sub)) {
            if (sceIoMkdir(sub.c_str(), 0777) < 0) return false;
        }
        i = j + 1;
    }
    return true;
}
bool KfeFileOps::isDirectoryPath(const std::string& path) {
    SceIoStat st{}; if (sceIoGetstat(path.c_str(), &st) < 0) return false;
    return FIO_S_ISDIR(st.st_mode);
}
// Replace your copyFile with this hardened version.
// NOTE: signature unchanged from your current integration that passes `this`.
bool KfeFileOps::copyFile(const std::string& src, const std::string& dst, KernelFileExplorer* self) {
    logf("copyFile: %s -> %s", src.c_str(), dst.c_str());

    const bool verifyCritical = kfeNeedsDestPresenceVerify(src) || kfeNeedsDestPresenceVerify(dst);

    auto runCopyPass = [&](int pass)->bool {
        SceUID in = sceIoOpen(src.c_str(), PSP_O_RDONLY, 0);
        if (in < 0) { logf("  open src failed %d", in); return false; }

        // Ensure parent
        std::string parent = parentOf(dst);
        if (!ensureDirRecursive(parent)) {
            logf("  ensureDirRecursive(%s) failed", parent.c_str());
            sceIoClose(in);
            return false;
        }

        SceUID out = sceIoOpen(dst.c_str(), PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0666);
        if (out < 0) { logf("  open dst failed %d", out); sceIoClose(in); return false; }

        uint64_t fileSize = 0;
        { SceIoStat st{}; if (sceIoGetstat(src.c_str(), &st) >= 0) fileSize = (uint64_t)st.st_size; if (!fileSize) fileSize = 1; }

        if (self && self->msgBox) { self->msgBox->showProgress(basenameOf(src).c_str(), 0, fileSize); self->renderOneFrame(); }

        size_t readBuf = 512 * 1024;
        uint8_t* buf = (uint8_t*)malloc(readBuf);
        if (!buf) {
            readBuf = 128 * 1024;
            buf = (uint8_t*)malloc(readBuf);
        }
        if (!buf) {
            readBuf = 32 * 1024;
            buf = (uint8_t*)malloc(readBuf);
        }
        if (!buf) {
            logf("  alloc read buffer failed");
            sceIoClose(in);
            sceIoClose(out);
            return false;
        }
        logf("  read buffer = %u bytes (pass %d)", (unsigned)readBuf, pass);

        int maxWriteChunk  = 64  * 1024;   // start at 64 KiB, we may shrink on trouble
        const int MIN_WRITE_CHUNK = 4 * 1024;

        bool ok = true; uint64_t total = 0; int lastErr = 0;

        auto destDev = std::string(dst.substr(0, 4)); // "ms0:" / "ef0:" (dst is "ef0:/...")
        for (;;) {
            int r = sceIoRead(in, buf, (int)readBuf);
            if (r < 0) { lastErr = r; logf("  read err %d", r); ok = false; break; }
            if (r == 0) break;

            int off = 0;
            while (off < r) {
                int chunk = r - off;
                if (chunk > maxWriteChunk) chunk = maxWriteChunk;

                int w = sceIoWrite(out, buf + off, chunk);
                if (w <= 0) {
                    // If 0 or negative, try shrinking the chunk a few times before giving up
                    int attemptChunk = chunk;
                    for (int tries = 0; tries < 4 && w <= 0 && attemptChunk > MIN_WRITE_CHUNK; ++tries) {
                        attemptChunk >>= 1; // half it
                        sceKernelDelayThread(500);
                        w = sceIoWrite(out, buf + off, attemptChunk);
                        if (w > 0) {
                            maxWriteChunk = attemptChunk;
                            break;
                        }
                    }

                    if (w <= 0) {
                        uint64_t freeBytes = 0;
                        if (getFreeBytesCMF(destDev.c_str(), freeBytes)) {
                            logf("  write err %d (chunk=%d) with free=%llu bytes on %s",
                                w, attemptChunk, (unsigned long long)freeBytes, canonicalDev(destDev.c_str()));
                        } else {
                            logf("  write err %d (chunk=%d); CMF free space query failed", w, attemptChunk);
                        }
                        lastErr = w;
                        ok = false;
                        break;
                    }
                }

                off   += w;
                total += (uint64_t)w;

                if (self && self->msgBox) { self->msgBox->updateProgress(total, fileSize); self->renderOneFrame(); }
            }

            if (!ok) break;
            sceKernelDelayThread(0);
        }

        sceIoClose(in);
        sceIoClose(out);
        free(buf);

        if (!ok) {
            logf("copyFile: FAIL after %llu/%llu bytes (err=%d)",
                (unsigned long long)total, (unsigned long long)fileSize, lastErr);
            sceIoRemove(dst.c_str()); // remove partial
            if (self && self->msgBox) { self->msgBox->updateProgress(total, fileSize); self->renderOneFrame(); }
            return false;
        }

        if (self && self->msgBox) { self->msgBox->updateProgress(fileSize, fileSize); self->renderOneFrame(); }
        logf("copyFile: OK %llu bytes", (unsigned long long)total);
        return true;
    };

    for (int pass = 1; pass <= 2; ++pass) {
        if (!runCopyPass(pass)) {
            if (!verifyCritical) return false;
            logf("copyFile: critical copy pass %d failed: %s -> %s", pass, src.c_str(), dst.c_str());
            if (pass == 2) {
                sKfeCriticalGuardFailure = true;
                return false;
            }
            sceKernelDelayThread(2 * 1000);
            continue;
        }
        if (!verifyCritical || kfeWaitForPathPresence(dst)) return true;
        logf("copyFile: critical destination missing after pass %d: %s", pass, dst.c_str());
        if (pass == 2) {
            sKfeCriticalGuardFailure = true;
            sceIoRemove(dst.c_str());
            return false;
        }
        sceIoRemove(dst.c_str());
        sceKernelDelayThread(2 * 1000);
    }
    return false;
}


bool KfeFileOps::removeDirRecursive(const std::string& dir) {
    SceUID d = kfeIoOpenDir(dir.c_str());
    if (d < 0) { logf("removeDirRecursive: open %s failed %d", dir.c_str(), d); return false; }
    SceIoDirent ent; memset(&ent, 0, sizeof(ent));
    while (kfeIoReadDir(d, &ent) > 0) {
        if (!strcmp(ent.d_name, ".") || !strcmp(ent.d_name, "..")) { memset(&ent,0,sizeof(ent)); continue; }
        std::string child = joinDirFile(dir, ent.d_name);
        if (FIO_S_ISDIR(ent.d_stat.st_mode)) {
            removeDirRecursive(child);
            sceIoRmdir(child.c_str());
        } else {
            sceIoRemove(child.c_str());
        }
        memset(&ent, 0, sizeof(ent));
        sceKernelDelayThread(0); // yield
    }
    kfeIoCloseDir(d);
    return sceIoRmdir(dir.c_str()) >= 0;
}
bool KfeFileOps::copyDirRecursive(const std::string& src, const std::string& dst, KernelFileExplorer* self) {
    logf("copyDirRecursive: %s -> %s", src.c_str(), dst.c_str());
    if (!ensureDirRecursive(dst)) { logf("  ensureDirRecursive failed"); return false; }
    SceUID d = kfeIoOpenDir(src.c_str());
    if (d < 0) { logf("  open src failed %d", d); return false; }

    SceIoDirent ent; memset(&ent, 0, sizeof(ent));
    bool ok = true;
    while (ok && kfeIoReadDir(d, &ent) > 0) {
        if (!strcmp(ent.d_name, ".") || !strcmp(ent.d_name, "..")) { memset(&ent,0,sizeof(ent)); continue; }
        if (kfeIsMacJunkName(ent.d_name)) { memset(&ent,0,sizeof(ent)); continue; }
        std::string s = joinDirFile(src, ent.d_name);
        std::string t = joinDirFile(dst, ent.d_name);

        if (FIO_S_ISDIR(ent.d_stat.st_mode)) {
            ok = ensureDir(t) && copyDirRecursive(s, t, self);
        } else {
            logf("  file: %s -> %s (%ld bytes)", s.c_str(), t.c_str(), (long)ent.d_stat.st_size);
            ok = copyFile(s, t, self);
        }
        memset(&ent, 0, sizeof(ent));
        sceKernelDelayThread(0); // yield
    }
    kfeIoCloseDir(d);
    if (!ok) {
        // Avoid leaving half-copied directories behind on failure.
        removeDirRecursive(dst);
    }
    logf("copyDirRecursive: %s", ok ? "OK" : "FAIL");
    return ok;
}

// Determine subroot for a given item path (preserve source tree)
std::string KfeFileOps::subrootFor(const std::string& path, GameItem::Kind kind) {
    // EBOOT trees to check
    const char* gameSubs[] = {"PSP/GAME/PSX/","PSP/GAME/Utility/","PSP/GAME150/","PSP/GAME/"};
    // ISO trees to check
    const char* isoSubs[]  = {"ISO/"};
    size_t pos = 0;
    if (path.size() >= 5) pos = 5; // after "ms0:/" or "ef0:/"
    if (kind == GameItem::EBOOT_FOLDER) {
        for (auto s : gameSubs) {
            if (path.size() >= pos + strlen(s) && strncasecmp(path.c_str()+pos, s, strlen(s))==0)
                return std::string(s);
        }
        return "PSP/GAME/"; // default fallback
    } else {
        for (auto s : isoSubs) {
            if (path.size() >= pos + strlen(s) && strncasecmp(path.c_str()+pos, s, strlen(s))==0)
                return std::string(s);
        }
        return "ISO/"; // default fallback
    }
}

bool KfeFileOps::parseCategoryFromPath(const std::string& pathAfterSubroot, std::string& outCat, std::string& outLeaf) {
    // pathAfterSubroot examples:
    //   "CAT_02PS1/SLUS12345", "Emulators/CPS1 (Capcom Play System 1)", "SLUS12345", "CAT_01PSP/Game.iso"
    size_t slash = pathAfterSubroot.find('/');
    if (slash == std::string::npos) {
        // No category component
        outCat.clear();
        outLeaf = pathAfterSubroot;
        return false;
    }
    // Treat the first segment as the category regardless of prefix
    outCat  = pathAfterSubroot.substr(0, slash);
    outLeaf = pathAfterSubroot.substr(slash + 1);
    return true;
}


std::string KfeFileOps::afterSubroot(const std::string& full, const std::string& subroot) {
    // full: "ms0:/PSP/GAME/CAT_foo/Leaf", subroot: "PSP/GAME/"
    if (full.size() < 5) return "";
    size_t pos = 5;
    if (full.size() >= pos + subroot.size() && strncasecmp(full.c_str()+pos, subroot.c_str(), subroot.size()) == 0) {
        return full.substr(pos + subroot.size());
    }
    // if not matching (weird), return whatever is after root
    return full.substr(5);
}

// Build destination path given a source and a destination device/category
std::string KfeFileOps::buildDestPath(const std::string& srcPath,
                                 GameItem::Kind kind,
                                 const std::string& destDevice,
                                 const std::string& destCategory /* "" for Uncategorized */) {
    std::string sub = subrootFor(srcPath, kind);
    std::string tail = afterSubroot(srcPath, sub);
    std::string srcCat, leaf;
    parseCategoryFromPath(tail, srcCat, leaf); // leaf is basename for folder/file
    std::string dest = destDevice + sub;
    if (!destCategory.empty() && strcasecmp(destCategory.c_str(),"Uncategorized")!=0) {
        dest = joinDirFile(dest, destCategory.c_str());
    }
    return joinDirFile(dest, leaf.c_str());
}

int KfeFileOps::kfeFastMoveDevctl(const char* src, const char* dst) {
    // Must be the same device: e.g. "ms0:/..." → both match up to ':'
    const char* c1 = strchr(src,  ':');
    const char* c2 = strchr(dst,  ':');
    if (!c1 || !c2) return -1;
    if ((c1 - src) != (c2 - dst)) return -1;
    for (const char* a = src, *b = dst; a <= c1; ++a, ++b) {
        if (*a != *b) return -1;
    }

    // Build device string "ms0:" or "ef0:"
    char dev[8]; size_t n = 0;
    do {
        if (n + 1 >= sizeof(dev)) return -1;
        dev[n] = src[n];
    } while (src[n++] != ':');
    dev[n] = '\0';

    // Devctl expects two pointers to the path parts AFTER the colon
    uint32_t data[2];
    data[0] = (uint32_t)(c1 + 1);
    data[1] = (uint32_t)(c2 + 1);

    // 0x02415830 = FAT intra-volume move/rename (instant)
    return pspIoDevctl(dev, 0x02415830, data, sizeof(data), nullptr, 0);
}

// Execute move for one path
bool KfeFileOps::moveOne(const std::string& src, const std::string& dst, GameItem::Kind kind, KernelFileExplorer* self) {
    logf("moveOne: src=%s", src.c_str());
    logf("        dst=%s", dst.c_str());
    logf("        kind=%s", (kind==GameItem::ISO_FILE)?"ISO":"EBOOT");
    const bool verifyCritical = kfeNeedsDestPresenceVerify(src) || kfeNeedsDestPresenceVerify(dst);

    if (!strcasecmp(src.c_str(), dst.c_str())) {
        logf("  src == dst; skip");
        return true;
    }

    // Make sure the destination parent exists
    std::string dstParent = parentOf(dst);
    if (!ensureDirRecursive(dstParent)) { logf("  ensureDirRecursive(parent) FAILED"); return false; }

    // Remove any case-variant collisions first (BOOT vs boot, etc).
    if (REPLACE_ON_MOVE) kfeRemoveCaseCollisionsInDir(dstParent, basenameOf(dst));

    // Replace policy: if a destination exists and we're replacing, clear it first
    SceIoStat dstSt{};
    if (pathExists(dst, &dstSt) && REPLACE_ON_MOVE) {
        if (isDirMode(dstSt)) { logf("  dst exists (dir) -> removing"); removeDirRecursive(dst); }
        else                  { logf("  dst exists (file)-> removing"); sceIoRemove(dst.c_str()); }
    }

    // Same device? Prefer instant operations.
    if (sameDevice(src, dst)) {
        int rc = kfeFastMoveDevctl(src.c_str(), dst.c_str());
        if (rc >= 0) {
            if (!verifyCritical || kfeWaitForPathPresence(dst)) return true;
            logf("  critical destination missing after fast move, retrying once: %s", dst.c_str());
            if (pathExists(src)) rc = kfeFastMoveDevctl(src.c_str(), dst.c_str());
            if (kfeWaitForPathPresence(dst)) return true;
            sKfeCriticalGuardFailure = true;
            return false;
        }

        if (kind == GameItem::ISO_FILE) {
            int rr = sceIoRename(src.c_str(), dst.c_str());
            if (rr >= 0) {
                if (!verifyCritical || kfeWaitForPathPresence(dst)) return true;
                logf("  critical destination missing after rename, retrying once: %s", dst.c_str());
                if (pathExists(src)) rr = sceIoRename(src.c_str(), dst.c_str());
                if (kfeWaitForPathPresence(dst)) return true;
                sKfeCriticalGuardFailure = true;
                return false;
            }
            // Fallback: same-device copy+delete -> show progress
            bool ok = copyFile(src, dst, self);
            if (ok) sceIoRemove(src.c_str());
            return ok;
        } else {
            int rr = sceIoRename(src.c_str(), dst.c_str());
            if (rr >= 0) return true;

            if (fastMoveDirByRenames(src, dst)) { sceIoRmdir(src.c_str()); return true; }

            // Last resort: show progress per file while copying the tree
            bool ok = copyDirRecursive(src, dst, self) && removeDirRecursive(src);
            return ok;
        }
    }

    // Cross-device: always copy+delete, with progress
    if (kind == GameItem::ISO_FILE) {
        bool ok = copyFile(src, dst, self);
        if (ok) sceIoRemove(src.c_str());
        return ok;
    } else {
        bool ok = copyDirRecursive(src, dst, self) && removeDirRecursive(src);
        return ok;
    }
}

// --- Delete helpers: show filename + a simple per-item progress ---

// Per-file delete with progress label
bool KfeFileOps::removeFileWithProgress(const std::string& path, KernelFileExplorer* self) {
    if (self && self->msgBox) {
        self->msgBox->showProgress(basenameOf(path).c_str(), 0, 1);
        self->renderOneFrame();
    }
    int rc = sceIoRemove(path.c_str());
    if (self && self->msgBox) {
        self->msgBox->updateProgress(1, 1);
        self->renderOneFrame();
    }
    return rc >= 0;
}

// Recursive folder delete that shows the current child name as we go
bool KfeFileOps::removeDirRecursiveProgress(const std::string& dir, KernelFileExplorer* self) {
    SceUID d = kfeIoOpenDir(dir.c_str());
    if (d < 0) {
        // If open fails, try removing the directory itself (may already be empty/inaccessible)
        if (self && self->msgBox) {
            self->msgBox->showProgress(basenameOf(dir).c_str(), 0, 1);
            self->renderOneFrame();
        }
        bool ok = (sceIoRmdir(dir.c_str()) >= 0);
        if (self && self->msgBox) {
            self->msgBox->updateProgress(1, 1);
            self->renderOneFrame();
        }
        return ok;
    }

    bool ok = true;
    SceIoDirent ent; memset(&ent, 0, sizeof(ent));
    while (ok && kfeIoReadDir(d, &ent) > 0) {
        if (!strcmp(ent.d_name, ".") || !strcmp(ent.d_name, "..")) {
            memset(&ent, 0, sizeof(ent));
            continue;
        }
        std::string child = joinDirFile(dir, ent.d_name);

        // Update label to current child before removing
        if (self && self->msgBox) {
            self->msgBox->showProgress(ent.d_name, 0, 1);
            self->renderOneFrame();
        }

        if (FIO_S_ISDIR(ent.d_stat.st_mode)) {
            // Recurse into subdir first
            ok = removeDirRecursiveProgress(child, self);
            if (ok) ok = (sceIoRmdir(child.c_str()) >= 0);
        } else {
            ok = (sceIoRemove(child.c_str()) >= 0);
        }

        // Mark this item finished
        if (self && self->msgBox) {
            self->msgBox->updateProgress(1, 1);
            self->renderOneFrame();
        }

        memset(&ent, 0, sizeof(ent));
        sceKernelDelayThread(0); // yield
    }
    kfeIoCloseDir(d);

    // Finally remove the now-empty parent directory itself (show its name as a cue)
    if (ok) {
        if (self && self->msgBox) {
            self->msgBox->showProgress(basenameOf(dir).c_str(), 0, 1);
            self->renderOneFrame();
        }
        ok = (sceIoRmdir(dir.c_str()) >= 0);
        if (self && self->msgBox) {
            self->msgBox->updateProgress(1, 1);
            self->renderOneFrame();
        }
    }

    return ok;
}

// Delete one path (file or folder) with UI updates
bool KfeFileOps::deleteOne(const std::string& path, GameItem::Kind kind, KernelFileExplorer* self) {
    if (kind == GameItem::ISO_FILE) {
        return removeFileWithProgress(path, self);
    } else {
        // EBOOT folder or any directory-like entry
        return removeDirRecursiveProgress(path, self);
    }
}

// Full delete executor: mirrors performMove()/performCopy() style and refresh rules
void KfeFileOps::performDelete(KernelFileExplorer* self) {
    if (!self) return;
    ClockGuard cg; cg.boost333();

    // Open progress box (no icon, centered bar + two text lines)
    self->msgBox = new MessageBox("Deleting...", nullptr, SCREEN_WIDTH, SCREEN_HEIGHT, 1.0f, 0, "", 16, 18, 8, 14);
    self->renderOneFrame();

    int ok = 0, fail = 0;
    std::vector<std::string> deletedPaths;
    deletedPaths.reserve(self->opSrcPaths.size());
    for (size_t i = 0; i < self->opSrcPaths.size(); ++i) {
        const std::string& p = self->opSrcPaths[i];
        const GameItem::Kind k = self->opSrcKinds[i];

        // NEW: show the cached game title (headline above the progress bar).
        // Falls back to basename when no title is cached.
        if (self->msgBox) {
            std::string title = self->getCachedTitleForPath(p);
            if (title.empty()) title = basenameOf(p);
            self->msgBox->setProgressTitle(title.c_str());
            self->renderOneFrame();
        }

        // Filename detail is already handled inside deleteOne(...) via showProgress/updateProgress.
        bool okOne = deleteOne(p, k, self);
        if (okOne) {
            ok++;
            self->checked.erase(p);
            deletedPaths.push_back(p);
        } else {
            fail++;
        }
        sceKernelDelayThread(0);
    }

    delete self->msgBox; self->msgBox = nullptr;

    // Remove hidden filters for any paths we actually deleted
    if (!deletedPaths.empty()) {
        self->removeHiddenAppFiltersForPaths(deletedPaths);
    }

    // Mutate the cache and stay right where we are — instantly.
    // 1) Ensure cache entry exists (scan once if truly missing)
    auto &dc = self->deviceCache[self->rootPrefix(self->currentDevice)];
    if (dc.snap.flatAll.empty() && dc.snap.uncategorized.empty()
        && dc.snap.categories.empty() && dc.snap.categoryNames.empty()) {
        self->scanDevice(self->currentDevice);              // one-time build
        self->snapshotCurrentScan(dc.snap);
    }
    // 2) Remove each source path from the snapshot
    for (const auto& p : self->opSrcPaths) {
        self->snapErasePath(dc.snap, p);
    }
    dc.dirty = false;                           // snapshot is authoritative

    // 3) Repaint from snapshot without any scan
    self->restoreScan(dc.snap);
    if (self->hasCategories) {
        if (self->view == KernelFileExplorer::View_CategoryContents) self->openCategory(self->currentCategory);
        else self->buildCategoryRows();
    } else {
        self->rebuildFlatFromCache();
    }

    // Feedback toast, like Move/Copy
    char res[64];
    if (fail == 0) {
        snprintf(res, sizeof(res), "Deleted %d item(s)", ok);
        self->drawMessage(res, COLOR_GREEN);
    } else if (ok == 0) {
        snprintf(res, sizeof(res), "Delete failed (%d)", fail);
        self->drawMessage(res, COLOR_RED);
    } else {
        snprintf(res, sizeof(res), "Deleted %d, failed %d", ok, fail);
        self->drawMessage(res, COLOR_YELLOW);
    }
    sceKernelDelayThread(800 * 1000);

    // Clear sentinel/op state used to piggyback confirm close
    self->opDestDevice.clear();
    self->opSrcPaths.clear(); self->opSrcKinds.clear();
    self->opSrcCount = 0;
    self->opSrcTotalBytes = 0;
    self->opPhase = KernelFileExplorer::OP_None;
}



bool KfeFileOps::copyOne(const std::string& src, const std::string& dst, GameItem::Kind kind, KernelFileExplorer* self) {
    logf("copyOne: %s -> %s (%s)", src.c_str(), dst.c_str(), (kind==GameItem::ISO_FILE)?"ISO":"EBOOT");
    std::string dstParent = parentOf(dst);
    if (!ensureDirRecursive(dstParent)) return false;

    // Remove any case-variant collisions first (BOOT vs boot, etc).
    if (REPLACE_ON_MOVE) kfeRemoveCaseCollisionsInDir(dstParent, basenameOf(dst));

    SceIoStat dstSt{};
    if (pathExists(dst, &dstSt) && REPLACE_ON_MOVE) {
        if (isDirMode(dstSt)) removeDirRecursive(dst);
        else sceIoRemove(dst.c_str());
    }

    if (kind == GameItem::ISO_FILE) {
        return copyFile(src, dst, self);
    } else {
        if (!copyDirRecursive(src, dst, self)) return false;

        // Audit folder copies and retry missing files a couple times.
        bool detailHiddenForVerify = false;
        if (self && self->msgBox) {
            self->msgBox->setMessage("Verifying...");
            self->msgBox->setProgressDetailVisible(false);
            detailHiddenForVerify = true;
            self->renderOneFrame();
        }

        auto finishVerify = [&](bool ok)->bool {
            if (detailHiddenForVerify && self && self->msgBox) {
                self->msgBox->setProgressDetailVisible(true);
            }
            return ok;
        };

        const int kRepairRetries = 2;
        for (int attempt = 0; attempt <= kRepairRetries; ++attempt) {
            bool scanOk = true;
            std::vector<KfeCopyPathPair> missing;
            kfeCollectMissingDestFiles(src, dst, missing, scanOk);
            if (scanOk && missing.empty()) return finishVerify(true);

            logf("copy audit: %s -> %s, attempt=%d, scanOk=%d, missing=%d",
                 src.c_str(), dst.c_str(), attempt, scanOk ? 1 : 0, (int)missing.size());

            if (attempt == kRepairRetries) {
                removeDirRecursive(dst);
                sKfeCriticalGuardFailure = true;
                return finishVerify(false);
            }

            // Retry only the missing files.
            for (size_t i = 0; i < missing.size(); ++i) {
                const std::string& ms = missing[i].src;
                const std::string& md = missing[i].dst;
                SceIoStat mdSt{};
                if (pathExists(md, &mdSt) && FIO_S_ISDIR(mdSt.st_mode)) {
                    removeDirRecursive(md);
                }
                if (!copyFile(ms, md, self)) {
                    logf("copy audit repair failed: %s -> %s", ms.c_str(), md.c_str());
                }
                sceKernelDelayThread(0);
            }
        }
        removeDirRecursive(dst);
        sKfeCriticalGuardFailure = true;
        return finishVerify(false);
    }
}
