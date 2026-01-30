#include <pspkernel.h>

#include "kfe_app.h"

PSP_MODULE_INFO("Homebrew Sorter Ultimate", 0x800, 1, 0);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER | THREAD_ATTR_VFPU);
PSP_HEAP_SIZE_KB(4096);

int main(int argc, char* argv[]) {
    const char* execPath = (argc > 0) ? argv[0] : nullptr;
    return RunKernelFileExplorer(execPath);
}
