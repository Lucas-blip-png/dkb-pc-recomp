/* What launcher.c shares with fetch.c: the catalog strings and small helpers, defined in launcher.c. MIT
   (LICENSE-MIT), as launcher.c. */
#ifndef DKB_LAUNCHER_H
#define DKB_LAUNCHER_H
#include <windows.h>

#include "launcher_text.h"

#define PATH_CHARS 1024
#define CMD_CHARS 8192
#define TEXT_CHARS 2048

extern int russian;
extern int chinese;
#define T(id) (chinese ? LAUNCHER_##id##_ZH : russian ? LAUNCHER_##id##_RU : LAUNCHER_##id##_EN)

/* tmpl with each {name} replaced by its value (pairs: name, value, ..., NULL); other text is copied as it is */
void fill(wchar_t *out, size_t cap, const wchar_t *tmpl, const wchar_t *const *pairs);
/* MessageBoxW with the launcher's title; a test build with DKB_LAUNCHER_LOG set writes text there and answers IDOK */
int ask(HWND owner, const wchar_t *text, UINT flags);
void say_with(HWND owner, const wchar_t *tmpl, const wchar_t *name, const wchar_t *value);
/* Whether a test build runs with DKB_LAUNCHER_LOG set (no window shows) */
int headless(void);
int is_file(const wchar_t *path);
int is_dir(const wchar_t *path);
void widen(const char *s, wchar_t *out, int cap);

#endif
