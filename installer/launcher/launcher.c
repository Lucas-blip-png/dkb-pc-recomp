/* DKB-Setup.exe and DKB-Launcher.exe, or TotK-Setup.exe: one program with two jobs (docs/installer.md, "Launcher and
   updates"). MIT (LICENSE-MIT): this file came from TotK Recomp and keeps its terms.

   Beside python\pythonw.exe (an unpacked release zip, or a version folder of the launcher) it is the setup: it starts
   python\pythonw.exe -m installer.gui with this exe's folder as the working directory and passes its own arguments on.
   That is all the TotK build does.

   Built with -DDKB_LAUNCHER (Bananza, tools/make_release.py build_launcher), anywhere else it is the launcher, the one
   file a player downloads (DKB-Launcher.exe on the release page). It keeps everything in DKB-Recomp beside itself
   (installer/update.py describes each file) and starts versions\<current.txt>\python\pythonw.exe -m installer.gui
   --launcher <this exe> --base <DKB-Recomp> and its own arguments; when that folder lacks its Python (an antivirus
   removed it), previous.txt's. The window does every update from then on, this exe's own replacement included. Only
   when no version is there (the first start) is one downloaded, by fetch.c (fetch_install_latest), which is adapted
   from GPL-3.0-or-later code and carries its own licence header; the TotK build neither compiles nor links it.
   manifest.c reads the release manifest. A leftover <this exe>.old (an update renames a running exe away) is deleted
   at start.

   Strings come from the catalog (installer/text/<lang>.json, ids launcher.* and dkb.launcher.*), with {name} holes that
   fill() fills; tools/make_release.py writes them into launcher_text.h, with LAUNCHER_REPO.
   A test build (-DDKB_LAUNCHER_TEST, never a release) reads DKB_LAUNCHER_BASE in place of the DKB-Recomp folder,
   DKB_LAUNCHER_MANIFEST and DKB_LAUNCHER_DOWNLOAD in place of the two GitHub links (http only to 127.0.0.1), and with
   DKB_LAUNCHER_LOG set shows no window: every message goes to that file and every question is answered yes.
   Build (tools/make_release.py build_launcher): llvm/bin/llvm-windres -I runtime -O coff launcher.rc -o launcher_rc.o,
   then llvm/bin/clang -O2 -mwindows -municode -Wl,--dependent-load-flag=0x800 launcher.c launcher_rc.o -o
   TotK-Setup.exe, and for Bananza clang ... -DDKB_LAUNCHER launcher.c manifest.c fetch.c launcher_rc.o -lwinhttp
   -lbcrypt -lcomctl32 -o DKB-Setup.exe. The flag (LOAD_LIBRARY_SEARCH_SYSTEM32) makes Windows load winhttp.dll and
   bcrypt.dll, which are not KnownDLLs, from System32 only, never from the folder the exe was downloaded to. */
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

#include "launcher.h"
#ifdef DKB_LAUNCHER
#include "fetch.h"
#include "manifest.h"
#endif

#define LANGID_RUSSIAN 0x19
#define LANGID_CHINESE 0x04

int russian;
int chinese;

void fill(wchar_t *out, size_t cap, const wchar_t *tmpl, const wchar_t *const *pairs)
{
    size_t n = 0;
    while (*tmpl && n + 1 < cap) {
        const wchar_t *end = *tmpl == L'{' ? wcschr(tmpl, L'}') : NULL;
        const wchar_t *value = NULL;
        for (const wchar_t *const *p = pairs; end && p && p[0]; p += 2)
            if (wcslen(p[0]) == (size_t)(end - tmpl - 1) && !wcsncmp(p[0], tmpl + 1, (size_t)(end - tmpl - 1))) {
                value = p[1];
                break;
            }
        if (!value) {
            out[n++] = *tmpl++;
            continue;
        }
        while (*value && n + 1 < cap) out[n++] = *value++;
        tmpl = end + 1;
    }
    out[n] = 0;
}

#ifdef DKB_LAUNCHER_TEST
static int test_log(wchar_t *path)
{
    DWORD n = GetEnvironmentVariableW(L"DKB_LAUNCHER_LOG", path, PATH_CHARS);
    return n && n < PATH_CHARS;
}
#endif

int headless(void)
{
#ifdef DKB_LAUNCHER_TEST
    wchar_t path[PATH_CHARS];
    return test_log(path);
#else
    return 0;
#endif
}

int ask(HWND owner, const wchar_t *text, UINT flags)
{
#ifdef DKB_LAUNCHER_TEST
    wchar_t path[PATH_CHARS];
    if (test_log(path)) {   /* one UTF-8 line per message, newlines as \n */
        char line[TEXT_CHARS * 3 + 2];
        int n = WideCharToMultiByte(CP_UTF8, 0, text, -1, line, sizeof line - 2, NULL, NULL);
        if (n <= 0) n = 1, line[0] = 0;
        for (char *p = line; *p; p++)
            if (*p == '\n') *p = '|';
        line[n - 1] = '\n', line[n] = 0;
        HANDLE f = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS, 0, NULL);
        if (f != INVALID_HANDLE_VALUE) {
            DWORD put;
            WriteFile(f, line, (DWORD)strlen(line), &put, NULL);
            CloseHandle(f);
        }
        return IDOK;
    }
#endif
    return MessageBoxW(owner, text, T(TITLE), flags);
}

void say_with(HWND owner, const wchar_t *tmpl, const wchar_t *name, const wchar_t *value)
{
    wchar_t text[TEXT_CHARS];
    const wchar_t *pairs[] = {name, value, NULL};
    fill(text, TEXT_CHARS, tmpl, pairs);
    ask(owner, text, MB_OK | MB_ICONERROR);
}

int is_file(const wchar_t *path)
{
    DWORD a = GetFileAttributesW(path);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

int is_dir(const wchar_t *path)
{
    DWORD a = GetFileAttributesW(path);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

void widen(const char *s, wchar_t *out, int cap)
{
    if (!MultiByteToWideChar(CP_UTF8, 0, s, -1, out, cap)) out[0] = 0;
}

/* 1 when args hold an option that runs the setup or the game with no window (installer/gui.py CLI_OPTIONS, and
   --play). A frontend such as RecompLauncher then reads the progress lines and waits for the exit code (#402). */
static int frontend_run(const wchar_t *args)
{
    static const wchar_t *const opts[] = {L"--dest", L"--source", L"--prepare", L"--nup", L"--exefs", L"--romfs",
                                          L"--keys", L"--update", L"--verify", L"--clean", L"--uninstall", L"--play"};
    for (size_t i = 0; i < sizeof opts / sizeof *opts; i++) {
        size_t n = wcslen(opts[i]);
        for (const wchar_t *p = wcsstr(args, opts[i]); p; p = wcsstr(p + 1, opts[i]))
            if ((p == args || p[-1] == L' ' || p[-1] == L'"') && (!p[n] || p[n] == L' ' || p[n] == L'=' || p[n] == L'"'))
                return 1;
    }
    return 0;
}

/* folder\python\pythonw.exe -m installer.gui <head> <args>, in folder. 0 when that Python is not there. With code,
   the child gets this process's standard handles and is waited for, and *code is its exit code. */
static int start_python(const wchar_t *folder, const wchar_t *head, const wchar_t *args, DWORD *code)
{
    wchar_t python[PATH_CHARS], cmd[CMD_CHARS];
    STARTUPINFOW si = {sizeof si};
    PROCESS_INFORMATION pi;
    if (swprintf(python, PATH_CHARS, L"%ls\\python\\pythonw.exe", folder) < 0 || !is_file(python)) return 0;
    if (swprintf(cmd, CMD_CHARS, L"\"%ls\" -m installer.gui%ls%ls%ls%ls", python, head[0] ? L" " : L"", head,
                 args[0] ? L" " : L"", args) < 0)
        return 0;
    if (code) {   /* a GUI exe still holds the pipes its caller gave it: pass them on so the progress lines get there */
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
        si.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
        si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    }
    if (!CreateProcessW(python, cmd, NULL, NULL, code != NULL, 0, NULL, folder, &si, &pi)) return 0;
    CloseHandle(pi.hThread);
    if (code) {
        WaitForSingleObject(pi.hProcess, INFINITE);
        if (!GetExitCodeProcess(pi.hProcess, code)) *code = 1;
    }
    CloseHandle(pi.hProcess);
    return 1;
}

#ifdef DKB_LAUNCHER
/* The version named by base\versions\<pointer>.txt */
static int start_installed(const wchar_t *base, const wchar_t *pointer, const wchar_t *head, const wchar_t *args)
{
    wchar_t path[PATH_CHARS], wtag[64];
    char tag[64];
    if (swprintf(path, PATH_CHARS, L"%ls\\versions\\%ls.txt", base, pointer) < 0) return 0;
    fetch_read_line(path, tag, sizeof tag);
    if (!valid_tag(tag)) return 0;
    widen(tag, wtag, 64);
    if (swprintf(path, PATH_CHARS, L"%ls\\versions\\%ls", base, wtag) < 0) return 0;
    return start_python(path, head, args, NULL);
}
#endif

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE previous, LPWSTR args, int show)
{
    wchar_t exe[PATH_CHARS], folder[PATH_CHARS];
    (void)inst, (void)previous, (void)show;
    LANGID display = GetUserDefaultUILanguage();
    russian = (display & 0x3ff) == LANGID_RUSSIAN;
    chinese = (display & 0x3ff) == LANGID_CHINESE;
    if (!args) args = L"";
    DWORD n = GetModuleFileNameW(NULL, exe, PATH_CHARS);
    if (n == 0 || n >= PATH_CHARS) { ask(NULL, T(PYTHON_MISSING), MB_OK | MB_ICONERROR); return 1; }
    wcscpy(folder, exe);
    wchar_t *slash = wcsrchr(folder, L'\\');
    if (slash) *slash = 0;

    /* the setup: an unpacked release folder */
    DWORD code = 0;
    if (start_python(folder, L"", args, frontend_run(args) ? &code : NULL)) return (int)code;
#ifndef DKB_LAUNCHER
    ask(NULL, T(PYTHON_MISSING), MB_OK | MB_ICONERROR);
    return 1;
#else
    /* the launcher */
    wchar_t base[PATH_CHARS], head[CMD_CHARS], old[PATH_CHARS], versions[PATH_CHARS];
    DWORD b = 0;
#ifdef DKB_LAUNCHER_TEST
    b = GetEnvironmentVariableW(L"DKB_LAUNCHER_BASE", base, PATH_CHARS);
#endif
    if (!b || b >= PATH_CHARS) swprintf(base, PATH_CHARS, L"%ls\\DKB-Recomp", folder);
    for (size_t len = wcslen(base); len > 3 && base[len - 1] == L'\\'; len--) base[len - 1] = 0;   /* "x\" would end the quote */
    /* Every path built below base (versions\<tag>.part\<top>\python\pythonw.exe is the longest, under 320 characters
       more) must fit PATH_CHARS: swprintf cuts a longer one short, and a cut path can name another folder of the base,
       which fetch.c's remove_tree would then delete */
    if (wcslen(base) > PATH_CHARS - 320) { say_with(NULL, T(WRITE_FAILED), L"path", base); return 1; }
    swprintf(old, PATH_CHARS, L"%ls.old", exe);
    DeleteFileW(old);   /* still running (an update moments ago): it goes next time */
    swprintf(head, CMD_CHARS, L"--launcher \"%ls\" --base \"%ls\"", exe, base);
    if (start_installed(base, L"current", head, args) || start_installed(base, L"previous", head, args)) return 0;
    if (!fetch_install_latest(inst, base)) return 1;   /* cancelled, or it said why */
    if (start_installed(base, L"current", head, args)) return 0;
    swprintf(versions, PATH_CHARS, L"%ls\\versions", base);
    say_with(NULL, T(START_FAILED), L"path", versions);
    return 1;
#endif
}
