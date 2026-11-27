// Shared by the system overrides: runtime/ov_system.c, ov_input.c, ov_media.c and ov_gfx.c, and their probe.
// oe_push_message, oe_set_focus and ov_nv_state are in gfx.h (the window posts focus messages, the NVN layer
// allocates through the recorded callbacks).
#pragma once
#include <stddef.h>
#include <stdint.h>

#include "gfx.h"

// ---- ov_nfp.c: the amiibo tag library for the menu (menu thread only). The folder the tags are looked for in; the
// folders below it that hold tag files ("" the top), sorted; the tag files of one folder (paths for DKB_AMIIBO, names
// without the extension). The pointers hold until the next call.
const char *ov_nfp_dir(void);
int ov_nfp_folders(const char **out, int max);
int ov_nfp_tags(const char *folder, const char **paths, const char **names, int max);

// ---- ov_system.c
int ov_handheld(void);                     // DKB_OPERATION_MODE=handheld (default docked); fatal on another value
const char *ov_language(void);             // the game's language, a Switch language code ("en-US", "zh-Hans", ...)
const char *ov_shared_font_dir(char *buf, size_t n);   // DKB_SHARED_FONTS, else <romfs>/../../firmware/shared-fonts
uint64_t ov_bfttf_size(const uint8_t *p, uint64_t len); // decoded font size of a BFTTF image, 0 if it is not one
