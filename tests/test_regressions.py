"""Run host-side fault-injection tests against the actual PSP state handlers.

Run in WSL: python3 tests/test_regressions.py
Only hardware APIs, drawing, file reads and navigation destinations are stubbed.
"""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


def source(name):
    return (ROOT / "app/src" / name).read_text(encoding="utf-8")


def between(text, start, end):
    return text.split(start, 1)[1].split(end, 1)[0]


class StateRegressions(unittest.TestCase):
    def run_cpp(self, text):
        with tempfile.TemporaryDirectory() as tmp:
            cpp = Path(tmp) / "test.cpp"
            binary = Path(tmp) / "test"
            cpp.write_text(text, encoding="utf-8")
            subprocess.run(["g++", "-std=c++11", "-O0", "-g", str(cpp), "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True)

    def test_category_rename_success_and_failure_reporting(self):
        handler = "static bool normalizeCategoryFolderForBase" + between(
            source("kfe_app_class_ui.h"), "static bool normalizeCategoryFolderForBase", "// Rename")
        self.run_cpp(r'''
#include <cassert>
#include <cstdarg>
#include <cstring>
#include <set>
#include <string>
#include <vector>
static std::set<std::string> dirs;
static bool failRename = false, failMerge = false;
static int merges = 0;
static bool dirExists(const std::string& p) { return dirs.count(p); }
static std::string joinDirFile(const std::string& root, const std::string& name) { return root + name; }
static bool isGameContentFolder(const std::string&) { return false; }
static bool isIsoVideoFolder(const std::string&, const std::string&) { return false; }
static std::string stripCategoryPrefixes(const std::string& s, bool) { return s.substr(6); }
static void logInit() {}
static void logClose() {}
static void logf(const char*, ...) {}
static bool renameIfExists(const std::string& root, const std::string& from, const std::string& to) {
    if (failRename) return false;
    dirs.erase(root + from); dirs.insert(root + to); return true;
}
static bool mergeCategoryFolders(const std::string& root, const std::string& from, const std::string& to) {
    ++merges;
    if (failMerge || !dirExists(root + from) || !dirExists(root + to)) return false;
    dirs.erase(root + from); return true;
}
''' + handler + r'''
int main() {
    dirs = {"root/CAT_01A"};
    assert(normalizeCategoryFolderForBase("root/", {"CAT_01A"}, "A", "CAT_02A", true));
    assert(dirExists("root/CAT_02A") && merges == 0); // Don't merge the already-renamed primary.
    dirs = {"root/CAT_01A"}; failRename = true;
    assert(!normalizeCategoryFolderForBase("root/", {"CAT_01A"}, "A", "CAT_02A", true));
    failRename = false;
    dirs = {"root/CAT_01A", "root/CAT_02A"};
    assert(normalizeCategoryFolderForBase("root/", {"CAT_01A", "CAT_02A"}, "A", "CAT_03A", true));
    assert(dirs.size() == 1 && dirExists("root/CAT_03A"));
    dirs = {"root/CAT_01A", "root/CAT_02A"}; failMerge = true;
    assert(!normalizeCategoryFolderForBase("root/", {"CAT_01A", "CAT_02A"}, "A", "CAT_03A", true));
}
''')

    def test_home_animation_recovers_from_io_and_memory_failures(self):
        text = source("kfe_app_types.h")
        globals_ = text.split("static const int CHECKBOX_PX", 1)[0]
        resources = "static Texture* loadStreamingFrameTexture" + between(
            text, "static Texture* loadStreamingFrameTexture", "// Collect frame info")
        handlers = "static bool loadHomeAnimationAt" + between(
            text, "static bool loadHomeAnimationAt", "static std::string currentHomeAnimCredit")
        path_handlers = "static bool remapResourcePathUnderFolder" + between(
            text, "static bool remapResourcePathUnderFolder", "static void disableHomeAnimationForUsb")
        self.run_cpp(r'''
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
struct Texture { void* data; int width; int height; };
struct MBAnimFrame { Texture* tex; uint32_t delayMs; };
static unsigned long long clockUs = 100;
static bool ioAvailable = false, invalidTexture = false;
static std::string validFramePrefix, execBase;
static std::vector<std::string> gPopAnimDirs;
static std::string gPopAnimLoadedDir;
static std::string currentExecBaseDir() { return execBase; }
static void setExecBaseOverrideFromAppDir(const std::string& path) { execBase = path; }
static int loads = 0, liveTextures = 0, textureBudget = 100;
static Texture* texLoadPNG(const char* path) {
    ++loads;
    if (!ioAvailable || liveTextures >= textureBudget) return nullptr;
    if (!validFramePrefix.empty() && std::string(path).compare(0, validFramePrefix.size(), validFramePrefix) != 0) return nullptr;
    ++liveTextures;
    return new Texture{invalidTexture ? nullptr : (void*)1, 10, 10};
}
static void texFree(Texture* tex) { assert(tex); --liveTextures; delete tex; }
static void bleedTextureAlpha(Texture*) {}
static void freeAnimationFrames(std::vector<MBAnimFrame>& frames) {
    for (auto& frame : frames) if (frame.tex) texFree(frame.tex);
    frames.clear();
}
static void clearHomeAnimChunks() {}
static void ensureHomeAnimChunkForIndex(size_t) {}
static void updateMsLedForHomeAnim() {}
static unsigned long long sceKernelGetSystemTimeWide() { return clockUs; }
static unsigned popAnimRand() { return 0; }
static unsigned long long frameDelayUs(const MBAnimFrame&, unsigned long long) { return 100; }
''' + globals_ + resources + r'''
static bool collectStreamingFrameInfo(const std::string& dir, std::vector<StreamFrameInfo>& info,
                                     unsigned long long& minDelay) {
    minDelay = 100;
    info = {{dir + "/frame0.png", 1}, {dir + "/frame1.png", 1}};
    return true;
}
static void collectHomeAnimations(const std::string&) { gHomeAnimEntries = {{"anim", "credit"}}; }
''' + path_handlers + handlers + r'''
int main() {
    gHomeAnimEntries = {{"anim", "credit"}};
    maybeStartHomeAnimation();
    assert(gHomeAnimIndex == -1 && liveTextures == 0);
    const int failedLoads = loads;
    ioAvailable = true;
    maybeStartHomeAnimation();
    assert(loads == failedLoads); // Backoff prevents retrying PNG loads every frame.
    clockUs += 2000001;
    maybeStartHomeAnimation();
    assert(gHomeAnimIndex == 0 && getCurrentHomeAnimTexture());

    suspendHomeAnimation();
    assert(gHomeAnimIndex == -1 && liveTextures == 0);
    maybeStartHomeAnimation(); // Releasing textures must leave a reloadable state.
    assert(gHomeAnimIndex == 0 && getCurrentHomeAnimTexture());

    suspendHomeAnimation();
    gHomeAnimPaused = true;
    gHomeAnimDeferFrames = -1; // Explicit USB suspension still blocks auto-start.
    const int beforeUsb = loads;
    clockUs += 4000000;
    maybeStartHomeAnimation();
    assert(loads == beforeUsb);
    armHomeAnimationForRoot();
    maybeStartHomeAnimation(); maybeStartHomeAnimation();
    assert(loads == beforeUsb);
    maybeStartHomeAnimation();
    assert(getCurrentHomeAnimTexture());

    suspendHomeAnimation();
    textureBudget = 1; // Cannot preload both frames: streaming must still work.
    maybeStartHomeAnimation();
    assert(gHomeAnimStreaming && liveTextures == 1 && getCurrentHomeAnimTexture());
    Texture* previous = getCurrentHomeAnimTexture();
    textureBudget = 2;
    invalidTexture = true;
    clockUs += 10000;
    advanceHomeAnimationFrame();
    assert(liveTextures == 1 && getCurrentHomeAnimTexture() == previous);
    resetHomeAnimationState(false);
    assert(liveTextures == 0);

    // Reproduce a category rename containing this executable. After releasing
    // textures, cached old filenames fail until the rename hook remaps them.
    invalidTexture = false; textureBudget = 100;
    const std::string oldCat = "ms0:/PSP/GAME/CAT_01Tools";
    const std::string newCat = "ms0:/PSP/GAME/CAT_02Tools";
    execBase = oldCat + "/Sorter/";
    validFramePrefix = execBase;
    gHomeAnimEntries = {{execBase + "resources/animations/anim", "credit"}};
    gPopAnimDirs = {gHomeAnimEntries[0].dir};
    gPopAnimLoadedDir = gPopAnimDirs[0];
    maybeStartHomeAnimation(); assert(getCurrentHomeAnimTexture());
    suspendHomeAnimation();
    validFramePrefix = newCat + "/Sorter/"; // Files now exist only at the renamed location.
    maybeStartHomeAnimation(); assert(!getCurrentHomeAnimTexture());
    remapExecResourcesForFolderRename(oldCat, newCat);
    assert(execBase == validFramePrefix);
    assert(gHomeAnimEntries[0].dir.find(newCat) == 0);
    assert(gPopAnimDirs[0].find(newCat) == 0 && gPopAnimLoadedDir.find(newCat) == 0);
    maybeStartHomeAnimation(); assert(getCurrentHomeAnimTexture()); // No USB rescan needed.
    assert(gHomeAnimStreamInfo[0].path.find(newCat) == 0);
    std::string sibling = oldCat + "Extra/Sorter/";
    assert(!remapResourcePathUnderFolder(sibling, oldCat, newCat));
    resetHomeAnimationState(false);
    assert(liveTextures == 0);
}
''')

    def test_unsaved_order_save_discard_cancel_and_failed_save(self):
        text = source("kfe_app_class_ui.h")
        handlers = "std::vector<std::string> savedListOrder;" + between(
            text, "std::vector<std::string> savedListOrder;", "// UI cache")
        resolve = "if (menuContext == MC_SaveOrder) {" + between(
            source("kfe_app_class_input.h"), "if (menuContext == MC_SaveOrder) {",
            "} else if (menuContext == MC_ContentOps)") + "}"
        self.run_cpp(r'''
#include <algorithm>
#include <cassert>
#include <cstring>
#include <string>
#include <vector>
static const char* kCatSettingsLabel = "Settings";
static const int SCREEN_WIDTH = 480, SCREEN_HEIGHT = 272;
struct SceCtrlData { unsigned Buttons = 0; };
static void sceCtrlReadBufferPositive(SceCtrlData*, int) {}
struct MessageBox {};
struct FileOpsItem { const char* name; bool disabled; };
struct FileOpsMenu {
    FileOpsMenu(const char*, const std::vector<FileOpsItem>& items, int, int, int, int, int = 0) {
        assert(items.size() == 3); // All exits offer Save, Discard and Cancel.
    }
    void primeButtons(unsigned) {}
};
struct Harness {
    enum View { View_Categories, View_CategoryContents, View_AllFlat } view = View_AllFlat;
    enum MenuContext { MC_ContentOps, MC_SaveOrder } menuContext = MC_ContentOps;
    enum ActionMode { AM_Move, AM_Copy };
    struct Row { char d_name[256]; };
    std::vector<Row> entries;
    std::vector<std::string> entryPaths;
    std::string currentCategory;
    std::vector<std::string> checked;
    bool catSortMode = false, catPickActive = false, inputWaitRelease = false, showRoots = false;
    int catPickIndex = -1, selectedIndex = 0, scrollOffset = 0, exits = 0, saves = 0;
    bool saveSucceeds = true;
    FileOpsMenu* fileMenu = nullptr;
    std::string stripCategoryPrefixes(const char* name) const {
        std::string s = name;
        if (s.compare(0, 4, "CAT_") == 0) s.erase(0, 4);
        if (s.size() > 2 && s[0] >= '0' && s[0] <= '9' && s[1] >= '0' && s[1] <= '9') s.erase(0, 2);
        return s;
    }
    void setCategorySortMode(bool enable) { catSortMode = enable; ++exits; }
    void openGclSettingsScreen() { ++exits; }
    void startAction(ActionMode) { ++exits; }
    void buildCategoryRows() { view = View_Categories; ++exits; }
    int categoryVisibleRows() const { return 8; }
    MessageBox* pushReturningModal() { return nullptr; }
    void buildRootRows() { showRoots = true; ++exits; }
    void popModal(MessageBox*) {}
    bool applyCategoryOrderAndPersist() { return commitOrderTimestamps(); }
    bool commitOrderTimestamps() { ++saves; if (saveSucceeds) rememberListOrder(); return saveSucceeds; }
''' + handlers + r'''
    void choose(int choice) {
        delete fileMenu; fileMenu = nullptr;
''' + resolve + r'''
    }
};
int main() {
    Harness h;
    h.entryPaths = {"a", "b"}; h.rememberListOrder();
    h.requestOrderExit(Harness::OE_Content);
    assert(h.exits == 1 && !h.fileMenu && h.saves == 0); // Unchanged: no prompt.

    h.exits = 0; h.showRoots = false;
    std::swap(h.entryPaths[0], h.entryPaths[1]);
    h.requestOrderExit(Harness::OE_Content);
    assert(h.fileMenu && h.exits == 0);
    h.choose(-1); // Circle cancels the exit, retaining order and selection.
    assert(!h.fileMenu && h.exits == 0 && h.entryPaths[0] == "b");
    h.requestOrderExit(Harness::OE_Content); h.choose(2); // Explicit Cancel.
    assert(h.exits == 0 && h.saves == 0);
    h.requestOrderExit(Harness::OE_Move); assert(h.fileMenu); h.choose(2);
    assert(h.exits == 0); // Entering another device/category picker also protects edits.

    h.requestOrderExit(Harness::OE_Content); h.saveSucceeds = false; h.choose(0);
    assert(h.exits == 0 && h.saves == 1);
    h.requestOrderExit(Harness::OE_Content); assert(h.fileMenu); // Failure stays dirty.
    h.saveSucceeds = true; h.choose(0);
    assert(h.exits == 1 && h.saves == 2 && h.savedListOrder == h.entryPaths);

    h.exits = 0;
    std::swap(h.entryPaths[0], h.entryPaths[1]);
    h.requestOrderExit(Harness::OE_Content); h.choose(1); // Discard navigates without saving.
    assert(h.exits == 1 && h.saves == 2);
    std::swap(h.entryPaths[0], h.entryPaths[1]); // Undo to last saved order.
    h.requestOrderExit(Harness::OE_Content); assert(!h.fileMenu);
    h.remapSavedListItem("a", "renamed-a");
    for (auto& path : h.entryPaths) if (path == "a") path = "renamed-a";
    h.requestOrderExit(Harness::OE_Content); assert(!h.fileMenu); // Rename isn't a reorder.

    Harness cats;
    cats.view = Harness::View_Categories; cats.catSortMode = true;
    cats.entries = {{{"Settings"}}, {{"CAT_01A"}}, {{"CAT_02B"}}, {{"Uncategorized"}}};
    cats.rememberListOrder();
    assert((cats.savedListOrder == std::vector<std::string>{"A", "B"}));
    std::swap(cats.entries[1], cats.entries[2]);
    cats.requestOrderExit(Harness::OE_SortMode); assert(cats.fileMenu);
    cats.choose(2); assert(cats.catSortMode && cats.exits == 0);
    cats.requestOrderExit(Harness::OE_Settings); assert(cats.fileMenu);
    cats.choose(-1); assert(cats.catSortMode && cats.exits == 0);
    cats.requestOrderExit(Harness::OE_SortMode); cats.choose(0);
    assert(!cats.catSortMode && cats.exits == 1 && cats.saves == 1);
    cats.entries[1] = {{"CAT_01B"}}; cats.entries[2] = {{"CAT_02A"}};
    assert(cats.savedListOrder == cats.visibleListOrder()); // Prefix renumbering isn't another edit.
    cats.requestOrderExit(Harness::OE_Categories); assert(!cats.fileMenu && cats.showRoots);
}
''')


if __name__ == "__main__":
    unittest.main(verbosity=2)
