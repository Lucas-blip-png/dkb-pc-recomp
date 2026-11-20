#include <stdio.h>
#include <string.h>
#include <windows.h>

#include "platform/platform.h"

int main(void) {
    DWORD pid;
    if (GetConsoleProcessList(&pid, 1) != 1 || pid != GetCurrentProcessId()) {
        fputs("expected a private console\n", stderr);
        return 1;
    }
    const char *why = plat_console_hide();
    if (why || GetConsoleProcessList(&pid, 1) != 0) {
        fprintf(stderr, "private console was not detached: %s\n", why ? why : "still attached");
        return 2;
    }
    char line[64];
    if (!fgets(line, sizeof line, stdin) || strcmp(line, "redirected input\n")) {
        fputs("stdin was not preserved\n", stderr);
        return 3;
    }
    puts("redirected output");
    fputs("redirected error\n", stderr);
    return 0;
}
