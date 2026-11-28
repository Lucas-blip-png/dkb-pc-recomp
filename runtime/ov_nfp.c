// SPDX-License-Identifier: GPL-3.0-or-later
// nn::nfp, the console's NFC figure (amiibo) library, answered from a tag dump of the player's own (#508).
//
// Bananza imports thirteen nn::nfp functions and no more: Initialize, Finalize, ListDevices, GetNpadId, GetDeviceState,
// AttachAvailabilityChangeEvent, AttachActivateEvent, AttachDeactivateEvent, StartDetection, StopDetection, Mount,
// Unmount and GetModelInfo. Its reader object (main+0x1913d74 and the functions after it, 3.2.0) attaches the activate
// and deactivate events, starts the detection, mounts the tag read-only (MountTarget 1), copies the 0x40-byte
// ModelInfo, unmounts and waits for the deactivate event. It never asks for the tag's ID, its owner or an application
// area, so only the figure's model is needed: eight bytes that are plain on the tag (pages 21 and 22). The rest of a
// real tag is encrypted with keys this code neither has nor needs, and nothing is ever written to a tag or its file.
//
// The Amiibo tab of the menu (F1) picks the tag from the files under DKB_AMIIBO_DIR (the amiibo folder next to the
// program unless set), by folder (ov_nfp_folders, ov_nfp_tags below).
// DKB_AMIIBO=<file> (dkb.ini: amiibo = <file>) is the tag at the reader: a dump of an NTAG215 as most tools write it
// (".bin": 540 bytes, or 532 or 572) or a Flipper Zero NFC file (".nfc": a "Page N: xx xx xx xx" line per page). The
// file is read each time the game starts looking for a tag, so the setting can change while the game runs. With the
// tag at the reader from the start, the game finds it as soon as it looks, reads it, and the tag counts as lifted off
// once the game unmounts it; the next time the game looks, it is tapped again. Unset, there is no NFC
// reader, as before: ListDevices reports no device and the game says so. With a path set the game sees a reader
// (reader_on); a file that is no figure's tag (tag_load logs why) leaves it searching until the player cancels.
//
// The file formats, the test for a figure's tag and the tag bytes ModelInfo is made from are Longshot's
// (https://git.magicbean.dev/MagicBeanDev/Longshot @ 86b3b9e4f0e198a74d5ff7afa9bbf717096a9c39, host/nn_nfp.c,
// GPL-3.0-or-later, Copyright (C) 2026 the Longshot contributors), written again for the calls Bananza makes: none of
// its lines is copied. No tag dump belongs in the repository: tests/nfp_tag_test.py builds a pretended tag in code.
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "cfg.h"
#include "host.h"
#include "ov.h"
#include "platform/platform.h"

// Results of module 115 (nfp): description << 9 | module.
#define NFP_R_DEVICE_NOT_FOUND 0x8073u   // 64: no NFC reader
#define NFP_R_WRONG_STATE 0x9273u        // 73: the call does not fit the reader's state
#define NFP_R_TAG_REMOVED 0xc273u        // 97: the tag left the reader

// nn::nfp::DeviceState
enum { ST_INIT, ST_SEARCHING, ST_FOUND, ST_REMOVED, ST_MOUNTED, ST_UNAVAILABLE };

#define TAG_BYTES 540          // an NTAG215's 135 pages
#define TAG_MODEL 0x54         // pages 21 and 22: the figure's model, plain
#define NFP_HANDLE 0x50464e01ull   // the one reader's DeviceHandle; any value but 0, which the game uses for "none"

static PlatMutex nfp_lock = PLAT_MUTEX_INIT;
static int32_t nfp_state;                 // nn::nfp::GetState: 1 between Initialize and Finalize
static int dev_state = ST_INIT;
static uint8_t tag[TAG_BYTES];
static char tag_name[96];
static uint64_t ev_activate, ev_deactivate;   // the game's SystemEventType objects, 0 when not attached

static int hexval(int ch) { return isdigit(ch) ? ch - '0' : isxdigit(ch) ? tolower(ch) - 'a' + 10 : -1; }

// A Flipper Zero NFC file: text, one "Page N: xx xx xx xx" line per page. 1 when at least 130 pages were read.
static int parse_flipper(const char *text, uint64_t len, uint8_t out[TAG_BYTES]) {
    int pages = 0;
    for (const char *p = text, *end = text + len; p < end;) {
        const char *eol = memchr(p, '\n', (size_t)(end - p));
        if (!eol) eol = end;
        unsigned page;
        int at = 0;
        if (eol - p > 6 && !memcmp(p, "Page ", 5) && sscanf(p, "Page %u:%n", &page, &at) == 1 && at &&
            page < TAG_BYTES / 4) {
            const char *q = p + at;
            uint8_t b[4];
            int n = 0;
            for (; n < 4; n++) {
                while (q < eol && *q == ' ') q++;
                if (eol - q < 2 || hexval((unsigned char)q[0]) < 0 || hexval((unsigned char)q[1]) < 0) break;
                b[n] = (uint8_t)(hexval((unsigned char)q[0]) << 4 | hexval((unsigned char)q[1]));
                q += 2;
            }
            if (n == 4) memcpy(out + page * 4, b, 4), pages++;
        }
        p = eol + 1;
    }
    return pages >= 130;
}

// 1 when out holds a figure's tag: an NTAG215 by its capability container, with a model that ends in 02.
static int tag_is_figure(const uint8_t t[TAG_BYTES]) {
    static const uint8_t cc[4] = {0xf1, 0x10, 0xff, 0xee};
    return !memcmp(t + 0x0c, cc, 4) && t[TAG_MODEL + 7] == 0x02;
}

static const char *base_name(const char *path) {
    const char *base = path;
    for (const char *p = path; *p; p++)
        if (*p == '/' || *p == '\\') base = p + 1;
    return base;
}

// Reads the tag file of DKB_AMIIBO into tag. 0 when the setting is empty or the file is not a figure's tag (logged
// once per path).
static int tag_load(void) {
    static char warned[512];
    const char *path = cfg_get("DKB_AMIIBO");
    if (!path || !*path) return 0;
    uint64_t size = 0;
    uint8_t *data = plat_file_read(path, &size);
    uint8_t t[TAG_BYTES] = {0};
    int ok = 0;
    const char *why = "it cannot be read";
    if (data) {
        size_t n = strlen(path);
        int flipper = n > 4 && (!strcmp(path + n - 4, ".nfc") || !strcmp(path + n - 4, ".NFC"));
        if (flipper) ok = parse_flipper((const char *)data, size, t), why = "it has fewer than 130 \"Page N:\" lines";
        else if (size == 540 || size == 532 || size == 572)
            memcpy(t, data, size < TAG_BYTES ? size : TAG_BYTES), ok = 1;
        else why = "a .bin tag dump has 540, 532 or 572 bytes";
        if (ok && !tag_is_figure(t)) ok = 0, why = "it is not a figure's NTAG215 tag";
        free(data);
    }
    if (!ok) {
        if (strncmp(warned, path, sizeof warned - 1)) {
            snprintf(warned, sizeof warned, "%s", path);
            rt_log("nfp: amiibo = %s is not used: %s", base_name(path), why);
        }
        return 0;
    }
    memcpy(tag, t, sizeof tag);
    snprintf(tag_name, sizeof tag_name, "%s", base_name(path));
    return 1;
}

// ---- the tag library, for the menu (ui/menu.cpp tab_amiibo). Menu thread only.
// DKB_AMIIBO_DIR, else the amiibo folder next to the program: tag files (.bin, .nfc) directly in it and in its
// subfolders, four levels deep, listed by folder. A file is only checked when the game reads it (tag_load).
#define LIB_MAX 8192
#define LIB_FOLDERS 512
// dir: the file's folder below the library, "" the top; name: the file's name without its extension
typedef struct { char *path, *dir, *name; } LibTag;
static LibTag lib[LIB_MAX];
static int nlib;
static char *lib_folder[LIB_FOLDERS];
static int nlib_folder;
static char lib_root[1024];
static uint64_t lib_scanned_ns;

const char *ov_nfp_dir(void) {
    static char dir[1024];
    const char *v = cfg_get("DKB_AMIIBO_DIR");
    if (v && *v) return snprintf(dir, sizeof dir, "%s", v), dir;
    char exe[1024];
    if (!plat_exe_path(exe, sizeof exe)) return "amiibo";
    char *cut = exe;
    for (char *p = exe; *p; p++)
        if (*p == '/' || *p == '\\') cut = p;
    *cut = 0;
    snprintf(dir, sizeof dir, "%s/amiibo", exe);
    return dir;
}

// 'b' for a ".bin" file, 'n' for a ".nfc" one (letters' case aside), 0 for another or a hidden one
static int tag_kind(const char *name) {
    size_t n = strlen(name);
    if (n < 5 || name[0] == '.' || name[n - 4] != '.') return 0;
    char e[4] = {(char)tolower((unsigned char)name[n - 3]), (char)tolower((unsigned char)name[n - 2]),
                 (char)tolower((unsigned char)name[n - 1]), 0};
    return !strcmp(e, "bin") ? 'b' : !strcmp(e, "nfc") ? 'n' : 0;
}

typedef struct { const char *rel; int depth; } LibWalk;
static void lib_walk(const char *rel, int depth);
static int lib_entry(const char *name, int kind, uint64_t size, void *ctx) {
    const LibWalk *w = ctx;
    char rel[1024];
    if (kind == PLAT_FS_DIR) {
        if (w->depth < 4 && name[0] != '.') {
            snprintf(rel, sizeof rel, "%s%s%s", w->rel, *w->rel ? "/" : "", name);
            lib_walk(rel, w->depth + 1);
        }
        return 1;
    }
    // A .bin of a tag dump's size, or a .nfc text file: what tag_load can take (installer/amiibo.py is_tag_file)
    int what = kind == PLAT_FS_FILE ? tag_kind(name) : 0;
    int sized = what == 'b' ? size == 540 || size == 532 || size == 572 : size > 0 && size <= 65536;
    // ponytail: at most 4096 files, the rest of a huge tree is not listed; raise it if a library needs more
    if (!what || !sized || nlib >= 4096) return 1;
    int n = (int)strlen(name) - 4;
    char path[2048];
    snprintf(path, sizeof path, "%s/%s%s%s", lib_root, w->rel, *w->rel ? "/" : "", name);
    LibTag *t = &lib[nlib];
    t->path = strdup(path), t->dir = strdup(w->rel), t->name = malloc((size_t)n + 1);
    if (!t->path || !t->dir || !t->name) rt_fatal("nfp: out of memory");
    memcpy(t->name, name, (size_t)n), t->name[n] = 0;
    nlib++;
    return 1;
}
static void lib_walk(const char *rel, int depth) {
    char dir[2048];
    snprintf(dir, sizeof dir, "%s%s%s", lib_root, *rel ? "/" : "", rel);
    LibWalk w = {rel, depth};
    plat_fs_list(dir, lib_entry, &w);
}
static int lib_cmp(const void *a, const void *b) {
    const LibTag *x = a, *y = b;
    int d = strcasecmp(x->dir, y->dir);
    return d ? d : strcasecmp(x->name, y->name);
}

// Reads the library again when its folder changed or two seconds passed (a file the player just added shows up).
static void lib_scan(void) {
    const char *root = ov_nfp_dir();
    uint64_t now = kern_now_ns();
    if (lib_scanned_ns && !strcmp(root, lib_root) && now - lib_scanned_ns < 2000000000ull) return;
    for (int i = 0; i < nlib; i++) free(lib[i].path), free(lib[i].dir), free(lib[i].name);
    nlib = nlib_folder = 0;
    snprintf(lib_root, sizeof lib_root, "%s", root);
    lib_walk("", 0);
    qsort(lib, (size_t)nlib, sizeof *lib, lib_cmp);
    for (int i = 0; i < nlib && nlib_folder < LIB_FOLDERS; i++)
        if (!nlib_folder || strcmp(lib_folder[nlib_folder - 1], lib[i].dir)) lib_folder[nlib_folder++] = lib[i].dir;
    lib_scanned_ns = now ? now : 1;
}

int ov_nfp_folders(const char **out, int max) {
    lib_scan();
    int n = nlib_folder < max ? nlib_folder : max;
    for (int i = 0; i < n; i++) out[i] = lib_folder[i];
    return n;
}

// No lib_scan() here: ov_nfp_folders (called first by the menu's rows_amiibo) refreshes the list, and a second scan
// could free the folder name the caller still holds.
int ov_nfp_tags(const char *folder, const char **paths, const char **names, int max) {
    int n = 0;
    for (int i = 0; i < nlib && n < max; i++)
        if (!strcmp(lib[i].dir, folder)) paths[n] = lib[i].path, names[n] = lib[i].name, n++;
    return n;
}

// 1 when the player set a tag file: the host then has an NFC reader.
static int reader_on(void) {
    const char *path = cfg_get("DKB_AMIIBO");
    return path && *path;
}

static int handle_ok(const Cpu *c, int i) {
    const uint64_t *h = arg_p(c, i);
    return reader_on() && h && *h == NFP_HANDLE;
}

static void ov_nfp_initialize(Cpu *c) {
    plat_xchg32(&nfp_state, 1);
    plat_mutex_lock(&nfp_lock);
    dev_state = ST_INIT, ev_activate = ev_deactivate = 0;
    plat_mutex_unlock(&nfp_lock);
    ret_x(c, 0);
}
static void ov_nfp_finalize(Cpu *c) {
    plat_xchg32(&nfp_state, 0);
    plat_mutex_lock(&nfp_lock);
    dev_state = ST_INIT, ev_activate = ev_deactivate = 0;
    plat_mutex_unlock(&nfp_lock);
    ret_x(c, 0);
}
static void ov_nfp_state(Cpu *c) { ret_x(c, (uint64_t)nfp_state); }

// Result ListDevices(DeviceHandle *out, int *count, int max)
static void ov_nfp_list(Cpu *c) {
    uint64_t *out = arg_p(c, 0);
    int32_t *count = arg_p(c, 1);
    int max = (int32_t)arg_x(c, 2);
    if (count) *count = 0;
    if (!reader_on() || !out || max < 1) return ret_x(c, NFP_R_DEVICE_NOT_FOUND);
    out[0] = NFP_HANDLE;
    if (count) *count = 1;
    ret_x(c, 0);
}

// Result AttachAvailabilityChangeEvent(os::SystemEventType *event)
static void ov_nfp_availability(Cpu *c) {
    uint64_t ev = arg_x(c, 0);
    if (!ev) rt_fatal("nn::nfp::AttachAvailabilityChangeEvent: null event");
    kern_bind_system_event(c, ev, 0);
    ret_x(c, 0);
}

// Result GetNpadId(hid::NpadIdType *out, const DeviceHandle &): the controller the reader is in
static void ov_nfp_npad(Cpu *c) {
    uint32_t *out = arg_p(c, 0);
    if (!handle_ok(c, 1) || !out) return ret_x(c, NFP_R_DEVICE_NOT_FOUND);
    *out = ov_handheld() ? 0x20u : 0u;
    ret_x(c, 0);
}

// Result AttachActivateEvent / AttachDeactivateEvent(os::SystemEventType *event, const DeviceHandle &): signalled when
// a tag comes to the reader and when it leaves. The game destroys both events when a detection ends and attaches them
// again for the next one, at the same addresses, so an earlier binding of the address is dropped first.
static void attach(Cpu *c, uint64_t *slot, const char *what) {
    uint64_t ev = arg_x(c, 0);
    if (!handle_ok(c, 1)) return ret_x(c, NFP_R_DEVICE_NOT_FOUND);
    if (!ev) rt_fatal("nn::nfp::%s: null event", what);
    kern_forget_system_event(ev);
    kern_bind_system_event(c, ev, 1);
    plat_mutex_lock(&nfp_lock);
    *slot = ev;
    plat_mutex_unlock(&nfp_lock);
    ret_x(c, 0);
}
static void ov_nfp_attach_activate(Cpu *c) { attach(c, &ev_activate, "AttachActivateEvent"); }
static void ov_nfp_attach_deactivate(Cpu *c) { attach(c, &ev_deactivate, "AttachDeactivateEvent"); }

// DeviceState GetDeviceState(const DeviceHandle &)
static void ov_nfp_device_state(Cpu *c) {
    if (!handle_ok(c, 0)) return ret_x(c, reader_on() ? ST_UNAVAILABLE : ST_INIT);
    plat_mutex_lock(&nfp_lock);
    int s = dev_state;
    plat_mutex_unlock(&nfp_lock);
    ret_x(c, (uint64_t)s);
}

// Result StartDetection(const DeviceHandle &): the tag file is at the reader, so it is found at once.
static void ov_nfp_start(Cpu *c) {
    if (!handle_ok(c, 0)) return ret_x(c, NFP_R_DEVICE_NOT_FOUND);
    int found = tag_load();
    plat_mutex_lock(&nfp_lock);
    dev_state = found ? ST_FOUND : ST_SEARCHING;
    uint64_t ev = found ? ev_activate : 0;
    plat_mutex_unlock(&nfp_lock);
    if (found) rt_log("nfp: the game looks for an amiibo: \"%s\" is at the reader", tag_name);
    else rt_log("nfp: the game looks for an amiibo: none at the reader");
    if (ev) kern_signal_system_event(ev);
    ret_x(c, 0);
}

// Result StopDetection(const DeviceHandle &)
static void ov_nfp_stop(Cpu *c) {
    if (!handle_ok(c, 0)) return ret_x(c, NFP_R_DEVICE_NOT_FOUND);
    plat_mutex_lock(&nfp_lock);
    // Leaving a found or mounted tag signals the deactivate event, as Eglantine's NfcDevice::StopDetection does by
    // closing the tag (CloseNfcTag)
    uint64_t ev = dev_state == ST_FOUND || dev_state == ST_MOUNTED ? ev_deactivate : 0;
    dev_state = ST_INIT;
    plat_mutex_unlock(&nfp_lock);
    if (ev) kern_signal_system_event(ev);
    ret_x(c, 0);
}

// Result Mount(const DeviceHandle &, ModelType, MountTarget)
static void ov_nfp_mount(Cpu *c) {
    if (!handle_ok(c, 0)) return ret_x(c, NFP_R_DEVICE_NOT_FOUND);
    plat_mutex_lock(&nfp_lock);
    uint32_t r = dev_state == ST_FOUND || dev_state == ST_MOUNTED ? 0
                 : dev_state == ST_REMOVED                        ? NFP_R_TAG_REMOVED
                                                                  : NFP_R_WRONG_STATE;
    if (!r) dev_state = ST_MOUNTED;
    plat_mutex_unlock(&nfp_lock);
    ret_x(c, r);
}

// Result Unmount(const DeviceHandle &): the game has what it wanted, and waits for the tag to leave the reader. The
// file stays where it is, so the tag is taken off here, as the player lifts a figure off the controller.
static void ov_nfp_unmount(Cpu *c) {
    if (!handle_ok(c, 0)) return ret_x(c, NFP_R_DEVICE_NOT_FOUND);
    plat_mutex_lock(&nfp_lock);
    uint32_t r = dev_state == ST_MOUNTED ? 0 : NFP_R_WRONG_STATE;
    uint64_t ev = 0;
    if (!r) dev_state = ST_REMOVED, ev = ev_deactivate;
    plat_mutex_unlock(&nfp_lock);
    if (ev) kern_signal_system_event(ev);
    ret_x(c, r);
}

// ModelInfo (0x40) is {characterId[3], seriesId, numberingId (u16), nfpType}: the tag has the type before the number
// and the series after it, and the number big-endian. With the bytes in the tag's order Bananza took the Donkey Kong
// and Pauline figure (series 0x1a) for one of an earlier series: it gave the KONG tiles but played the other figures'
// voice line. This byte order departs from Eglantine's nfp_types.h ModelInfo (type at byte 3, series at byte 6) on
// purpose: it matches the in-game result and libnx's NfpModelInfo.
static void model_fill(uint8_t m[0x40], const uint8_t *t) {
    memset(m, 0, 0x40);
    memcpy(m, t + TAG_MODEL, 3);                         // characterId[3]: the character and its variant
    m[3] = t[TAG_MODEL + 6];                             // seriesId
    m[4] = t[TAG_MODEL + 5], m[5] = t[TAG_MODEL + 4];    // numberingId, a native u16
    m[6] = t[TAG_MODEL + 3];                             // nfpType: figure, card, ...
}

// Result GetModelInfo(ModelInfo *out, const DeviceHandle &)
static void ov_nfp_model(Cpu *c) {
    uint8_t *m = arg_p(c, 0);
    if (!handle_ok(c, 1)) return ret_x(c, NFP_R_DEVICE_NOT_FOUND);
    plat_mutex_lock(&nfp_lock);
    uint32_t r = dev_state == ST_MOUNTED ? 0 : dev_state == ST_REMOVED ? NFP_R_TAG_REMOVED : NFP_R_WRONG_STATE;
    if (!r && m) model_fill(m, tag);
    plat_mutex_unlock(&nfp_lock);
    if (!r) rt_log("nfp: the game read the amiibo \"%s\" (model %02x%02x %02x %02x, number %02x%02x, series %02x)",
                   tag_name, tag[TAG_MODEL], tag[TAG_MODEL + 1], tag[TAG_MODEL + 2], tag[TAG_MODEL + 3],
                   tag[TAG_MODEL + 4], tag[TAG_MODEL + 5], tag[TAG_MODEL + 6]);
    ret_x(c, r);
}

static void ov_nfp_no_device(Cpu *c) { ret_x(c, NFP_R_DEVICE_NOT_FOUND); }

HOST_OVERRIDE("_ZN2nn3nfp10InitializeEv", ov_nfp_initialize);
HOST_OVERRIDE("_ZN2nn3nfp8FinalizeEv", ov_nfp_finalize);
HOST_OVERRIDE("_ZN2nn3nfp8GetStateEv", ov_nfp_state);
HOST_OVERRIDE("_ZN2nn3nfp11ListDevicesEPNS0_12DeviceHandleEPii", ov_nfp_list);
HOST_OVERRIDE("_ZN2nn3nfp29AttachAvailabilityChangeEventEPNS_2os15SystemEventTypeE", ov_nfp_availability);
HOST_OVERRIDE("_ZN2nn3nfp14GetDeviceStateERKNS0_12DeviceHandleE", ov_nfp_device_state);
HOST_OVERRIDE("_ZN2nn3nfp5MountERKNS0_12DeviceHandleENS0_9ModelTypeENS0_11MountTargetE", ov_nfp_mount);
HOST_OVERRIDE("_ZN2nn3nfp7UnmountERKNS0_12DeviceHandleE", ov_nfp_unmount);
HOST_OVERRIDE("_ZN2nn3nfp9GetNpadIdEPjRKNS0_12DeviceHandleE", ov_nfp_npad);
HOST_OVERRIDE("_ZN2nn3nfp19AttachActivateEventEPNS_2os15SystemEventTypeERKNS0_12DeviceHandleE", ov_nfp_attach_activate);
HOST_OVERRIDE("_ZN2nn3nfp21AttachDeactivateEventEPNS_2os15SystemEventTypeERKNS0_12DeviceHandleE",
              ov_nfp_attach_deactivate);
HOST_OVERRIDE("_ZN2nn3nfp14StartDetectionERKNS0_12DeviceHandleE", ov_nfp_start);
HOST_OVERRIDE("_ZN2nn3nfp13StopDetectionERKNS0_12DeviceHandleE", ov_nfp_stop);
HOST_OVERRIDE("_ZN2nn3nfp12GetModelInfoEPNS0_9ModelInfoERKNS0_12DeviceHandleE", ov_nfp_model);
// Bananza does not import it: the tag's ID is not given out.
HOST_OVERRIDE("_ZN2nn3nfp10GetTagInfoEPNS0_7TagInfoERKNS0_12DeviceHandleE", ov_nfp_no_device);
