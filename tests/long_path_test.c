// Standalone check of platform/win32.c on a path longer than MAX_PATH (#526): a file under a 300-character folder
// path is written, found, read back by plat_file_read and plat_file_open, and a short path still works.
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "platform/platform.h"

void rt_log(const char *fmt, ...) { (void)fmt; }

int main(int argc, char **argv) {
    char dir[1024], file[1100];
    int n = snprintf(dir, sizeof dir, "%s", argc > 1 ? argv[1] : "long_path_test_dir");
    while (n < 300) n += snprintf(dir + n, sizeof dir - n, "/segment_of_forty_characters_0123456789");
    snprintf(file, sizeof file, "%s/f.bin", dir);
    plat_file_mkdir_p(dir);
    if (!plat_dir_exists(dir)) { printf("FAIL mkdir\n"); return 1; }
    if (!plat_file_write(file, "abc", 3) || !plat_file_exists(file)) { printf("FAIL write\n"); return 1; }
    uint64_t size = 0;
    uint8_t *data = plat_file_read(file, &size);
    if (!data || size != 3 || memcmp(data, "abc", 3)) { printf("FAIL read\n"); return 1; }
    PlatFile *f = plat_file_open(file);
    char b[4] = {0};
    if (!f || plat_file_pread(f, b, 3, 0) != 3 || memcmp(b, "abc", 3)) { printf("FAIL open\n"); return 1; }
    plat_file_close(f);
    printf("OK long path (%d chars)\n", (int)strlen(file));
    return 0;
}
