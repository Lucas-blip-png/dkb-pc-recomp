// The add-on content RomFS: the supplied DLC, packed into an image and index like the game's own RomFS
// (tools/dkb_pack_romfs.py, tools/romfs_index.py). It is discovered as <exe folder>/aoc-<index>/ at first use and its
// index is parsed here at runtime rather than baked in, because romfs.c holds the base image's hash and size as
// generated constants and refuses a second mount name; this reader keeps that path untouched.
//
// One add-on content is mounted at a time: the game mounts what nn::aoc reports, and a second name is answered rather
// than silently replacing the first, so a wrong assumption shows up as a mount failure instead of wrong content.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "aoc_romfs.h"
#include "fs.h"
#include "host.h"
#include "platform/platform.h"
#include "romfs_path.h"

typedef struct { uint64_t offset, size; uint32_t name, len; } Row;   // a TOTKRFS1 row, as tools/romfs_index.py writes it

enum { AOC_SLOTS = 256 };

static char aoc_root[1800];             // the exe's folder: where aoc-<index> directories are looked for
static uint32_t aoc_ids[AOC_MAX];
static int aoc_n = -1;                  // -1: not scanned yet
static PlatMutex lock = PLAT_MUTEX_INIT;

// The mounted add-on content.
static char mount_name[32];
static uint32_t mount_index;
static uint8_t *idx;                    // the index bytes, read once at mount
static size_t idx_size;
static uint64_t rows, strings_off;
static PlatFile *image;
static uint64_t handles[AOC_SLOTS];     // index row + 1; 0: free

static void need(int c, const char *m) { if (!c) rt_fatal("aoc: %s", m); }

static Row row(uint64_t i) {
    Row r;
    memcpy(&r, idx + 24 + i * 24, sizeof r);
    return r;
}

static const char *row_name(Row r) { return (const char *)idx + strings_off + r.name; }

// The first row whose path is not below prefix, and whether that row starts with it.
static uint64_t lower_bound(const char *prefix, size_t n, int *matches) {
    uint64_t lo = 0, hi = rows;
    while (lo < hi) {
        uint64_t mid = lo + (hi - lo) / 2;
        if (strncmp(row_name(row(mid)), prefix, n) < 0) lo = mid + 1;
        else hi = mid;
    }
    *matches = lo < rows && !strncmp(row_name(row(lo)), prefix, n) && row(lo).len > n;
    return lo;
}

static uint64_t find(const char *path) {
    uint64_t lo = 0, hi = rows;
    while (lo < hi) {
        uint64_t mid = lo + (hi - lo) / 2;
        int c = strcmp(path, row_name(row(mid)));
        if (!c) return mid;
        if (c < 0) hi = mid;
        else lo = mid + 1;
    }
    return UINT64_MAX;
}

static void scan(void) {
    if (aoc_n >= 0) return;
    aoc_n = 0;
    char exe[1600];
    if (!plat_exe_path(exe, sizeof exe)) {
        rt_log("aoc: no executable path; add-on content unavailable");
        return;
    }
    char *cut = plat_last_sep(exe);
    if (!cut) return;
    *cut = 0;
    snprintf(aoc_root, sizeof aoc_root, "%s", exe);
    char path[1900];
    for (uint32_t i = 0; i < AOC_MAX && aoc_n < AOC_MAX; i++) {
        snprintf(path, sizeof path, "%s/aoc-%u/aoc-index.bin", aoc_root, i);
        if (!plat_file_exists(path)) continue;
        snprintf(path, sizeof path, "%s/aoc-%u/romfs.bin", aoc_root, i);
        if (!plat_file_exists(path)) continue;
        aoc_ids[aoc_n++] = i;
    }
    rt_log("aoc: %d add-on content%s next to the executable", aoc_n, aoc_n == 1 ? "" : "s");
}

uint32_t aoc_count(void) {
    scan();
    return (uint32_t)aoc_n;
}

uint32_t aoc_list(uint32_t *out, int offset, int count) {
    scan();
    if (!out || offset < 0 || count < 0) return 0;
    int n = 0;
    for (int i = offset; i < aoc_n && n < count; i++) out[n++] = aoc_ids[i];
    return (uint32_t)n;
}

const char *aoc_image_path(uint32_t index) {
    static char path[1900];
    scan();
    for (int i = 0; i < aoc_n; i++)
        if (aoc_ids[i] == index) {
            snprintf(path, sizeof path, "%s/aoc-%u/romfs.bin", aoc_root, index);
            return path;
        }
    return NULL;
}

int aoc_owns(const char *path) {
    size_t n = strlen(mount_name);
    return n && path && !strncmp(path, mount_name, n) && path[n] == ':';
}

int aoc_handle(uint64_t h) { return h > AOC_HANDLE_BASE && h < AOC_HANDLE_BASE + AOC_SLOTS; }

uint32_t aoc_mount(const char *name, uint32_t index) {
    if (!name || !*name || !romfs_path_valid_relative(name) || strlen(name) >= sizeof mount_name) return FS_R_INVALID;
    scan();
    int present = 0;
    for (int i = 0; i < aoc_n; i++)
        if (aoc_ids[i] == index) present = 1;
    if (!present) return FS_R_PATH_NOT_FOUND;   // no such add-on content on this machine
    plat_mutex_lock(&lock);
    if (*mount_name) {
        int same = mount_index == index && !strcmp(mount_name, name);
        plat_mutex_unlock(&lock);
        return same ? 0 : FS_R_ALREADY_EXISTS;
    }
    char path[1900];
    snprintf(path, sizeof path, "%s/aoc-%u/aoc-index.bin", aoc_root, index);
    uint64_t size = 0;
    idx = plat_file_read(path, &size);
    if (!idx) rt_fatal("aoc: add-on content %u has no readable index %s", index, path);
    need(size >= 24 && size < (64 << 20), "index size");
    idx_size = (size_t)size;
    need(!memcmp(idx, "TOTKRFS1", 8), "index format");
    memcpy(&rows, idx + 8, 8);
    memcpy(&strings_off, idx + 16, 8);
    need(rows < 1000000 && strings_off == 24 + rows * 24 && strings_off < idx_size, "index layout");
    snprintf(path, sizeof path, "%s/aoc-%u/romfs.bin", aoc_root, index);
    image = plat_file_open(path);
    if (!image) rt_fatal("aoc: add-on content %u has no readable image %s", index, path);
    PlatFileId id;
    need(plat_file_id(image, &id), "image size");
    for (uint64_t i = 0; i < rows; i++) {
        Row r = row(i);
        need(r.offset <= id.size && r.size <= id.size - r.offset &&
                 r.name < idx_size - strings_off && r.len > 1 && r.len <= idx_size - strings_off - r.name &&
                 row_name(r)[r.len - 1] == 0 && strlen(row_name(r)) == r.len - 1,
             "index row");
    }
    memset(handles, 0, sizeof handles);
    mount_index = index;
    snprintf(mount_name, sizeof mount_name, "%s", name);
    plat_mutex_unlock(&lock);
    rt_log("fs mount %s: -> add-on content %u, %llu files, %s", name, index, (unsigned long long)rows, path);
    return 0;
}

uint32_t aoc_unmount(const char *name) {
    if (!name || !*mount_name || strcmp(name, mount_name)) return FS_R_INVALID;
    plat_mutex_lock(&lock);
    if (*mount_name && image) plat_file_close(image);
    image = NULL;
    free(idx);
    idx = NULL;
    rows = strings_off = 0;
    *mount_name = 0;
    memset(handles, 0, sizeof handles);
    plat_mutex_unlock(&lock);
    rt_log("fs unmount %s: add-on content", name);
    return 0;
}

uint32_t aoc_open(uint64_t *out, const char *path, int mode) {
    if (!out || !aoc_owns(path)) return FS_R_INVALID;
    if (mode != FS_OPEN_READ) return FS_R_UNSUPPORTED_OPEN;   // add-on content is read-only
    const char *p = romfs_path_relative(mount_name, path);
    if (!p) return FS_R_PATH_NOT_FOUND;
    uint64_t i = find(p);
    if (i == UINT64_MAX) return FS_R_PATH_NOT_FOUND;
    plat_mutex_lock(&lock);
    uint32_t r = FS_R_INVALID;
    for (int j = 1; j < AOC_SLOTS; j++)
        if (!handles[j]) {
            handles[j] = i + 1;
            *out = AOC_HANDLE_BASE + (uint64_t)j;
            r = 0;
            break;
        }
    plat_mutex_unlock(&lock);
    if (r) rt_fatal("aoc: file handle table full (%d)", AOC_SLOTS);
    return r;
}

// The row a handle names, or 0.
static int source(uint64_t h, Row *r) {
    if (!aoc_handle(h)) return 0;
    plat_mutex_lock_shared(&lock);
    uint64_t i = handles[h - AOC_HANDLE_BASE];
    if (i && r && idx) *r = row(i - 1);
    plat_mutex_unlock_shared(&lock);
    return i != 0;
}

uint32_t aoc_read(uint64_t *out, uint64_t h, int64_t offset, void *buffer, uint64_t size) {
    Row r;
    if (!out || offset < 0 || !source(h, &r)) return FS_R_INVALID;
    if ((uint64_t)offset > r.size || (!buffer && size)) return FS_R_INVALID;
    uint64_t n = size < r.size - (uint64_t)offset ? size : r.size - (uint64_t)offset;
    uint64_t done = 0;
    while (done < n) {
        uint32_t chunk = n - done > (1u << 20) ? (1u << 20) : (uint32_t)(n - done);
        mem_thaw((uintptr_t)buffer + done, chunk);
        int64_t got = plat_file_pread(image, (uint8_t *)buffer + done, chunk, r.offset + (uint64_t)offset + done);
        if (got <= 0) rt_fatal("aoc: read failure at %#llx", (unsigned long long)(r.offset + offset + done));
        done += (uint64_t)got;
    }
    *out = done;
    return 0;
}

uint32_t aoc_size(int64_t *out, uint64_t h) {
    Row r;
    if (!out || !source(h, &r)) return FS_R_INVALID;
    *out = (int64_t)r.size;
    return 0;
}

void aoc_close(uint64_t h) {
    if (!aoc_handle(h)) rt_fatal("aoc: close of invalid file handle %#llx", (unsigned long long)h);
    plat_mutex_lock(&lock);
    handles[h - AOC_HANDLE_BASE] = 0;
    plat_mutex_unlock(&lock);
}

uint32_t aoc_entry_type(int *out, const char *path) {
    if (!out || !aoc_owns(path)) return FS_R_INVALID;
    const char *p = romfs_path_relative(mount_name, path);
    if (!p) return FS_R_PATH_NOT_FOUND;
    if (find(p) != UINT64_MAX) {
        *out = FS_ENTRY_FILE;
        return 0;
    }
    char dir[4200];
    if (snprintf(dir, sizeof dir, "%s/", p) >= (int)sizeof dir) return FS_R_INVALID;
    int matches = 0;
    lower_bound(dir, strlen(dir), &matches);
    if (!matches) return FS_R_PATH_NOT_FOUND;
    *out = FS_ENTRY_DIRECTORY;
    return 0;
}
