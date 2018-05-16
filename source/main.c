#include <stdio.h>

#include <malloc.h>
#include <switch.h>
#include <memory.h>
#include "menu.h"
#include "dumper.h"
#include "ccolor.h"
#include "filebrowser.h"

FsDeviceOperator fsOperatorInstance;

bool shouldExit = false;
bool shouldWaitForAnyButton = false;



void menuExit() {
    shouldExit = true;
}

void menuWaitForAnyButton() {
    printf(C_DIM "Press any button to return to menu\n");
    shouldWaitForAnyButton = true;
}

void startOperation(const char* title) {
    consoleClear();
    printf(C_DIM "%s\n\n" C_RESET, title);
}

void dumpPartitionZero(MenuItem* item) {
    startOperation("Raw Dump Partition 0 (SysUpdate)");
    workaroundPartitionZeroAccess(&fsOperatorInstance);
    dumpPartitionRaw(&fsOperatorInstance, 0);
    menuWaitForAnyButton();
}

void viewPartitionZero() {
    startOperation("Mount Partition 0 (SysUpdate)");
    workaroundPartitionZeroAccess(&fsOperatorInstance);
    FsFileSystem fs;
    if (!openPartitionFs(&fs, &fsOperatorInstance, 0)) {
        menuWaitForAnyButton();
        return;
    }
    if (fsdevMountDevice("test", fs) == -1) {
        printf("fsdevMountDevice failed\n");
        menuWaitForAnyButton();
        return;
    }
    printFilesInDir("test:/");
}


MenuItem mainMenu[] = {
        { .text = "Raw Dump Partition 0 (SysUpdate)", .callback = dumpPartitionZero },
        { .text = "View files on Game Card (SysUpdate)", .callback = viewPartitionZero },
        { .text = NULL }
};
void openMainMenu() {
    menuSetCurrent(mainMenu, menuExit);
}



int main(int argc, char **argv) {
    gfxInitDefault();
    consoleInit(NULL);

    if (R_FAILED(fsOpenDeviceOperator(&fsOperatorInstance))) {
        printf("Failed to open device operator\n");
        return -1;
    }
    openMainMenu();

    while(appletMainLoop())
    {
        bool btnWait = shouldWaitForAnyButton;

        hidScanInput();

        if (!btnWait)
            menuUpdate(&fsOperatorInstance);

        u64 kDown = hidKeysDown(CONTROLLER_P1_AUTO);
        if (kDown & KEY_PLUS) break;
        if (btnWait && kDown) {
            shouldWaitForAnyButton = false;
            menuPrint();
        }
        if (shouldExit)
            break;

        gfxFlushBuffers();
        gfxSwapBuffers();
        gfxWaitForVsync();
    }

    fsDeviceOperatorClose(&fsOperatorInstance);

    gfxExit();
    return 0;
}

