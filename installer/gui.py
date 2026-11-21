"""The installer window: python -m installer.gui

A front end over the CLI (docs/installer.md): it collects the game files, the keys file path, the game language and the
install folder, runs `python -m installer ...` as a subprocess and shows its JSON events. Decisions live in guilogic.py;
this file only draws. The window text is in installer/text/<en|ru|zh>.json under "ui.*" and "launcher.*" ids.

    --selftest [--shots DIR]   build every screen in each language and close by itself (PNG per screen with --shots)
    --lang en|ru|zh            window language (default: the Windows display language)

Started by the launcher (installer/launcher/launcher.c: --base <DKB-Recomp> --launcher <exe>) the window also has a
home screen: Play, the release check at start (off with the screen's tick box, --no-update-check or
DKB_NO_UPDATE_CHECK=1), the new release's notes and a one-click Update (installer/update.py). The update downloads and
checks the new tools, starts their window with --after-update and closes once it is up; that window rebuilds the
recorded install folder (only the steps whose inputs changed) and shows Play again. --allow-downgrade also offers an
older latest release.
"""
import argparse
import json
import os
import subprocess
import sys
import tempfile
import threading
import time
import tkinter as tk
import webbrowser
from pathlib import Path
from tkinter import filedialog, messagebox, ttk

try:
    from PIL import ImageGrab   # only the screenshots of --selftest --shots need it; the release has no PIL
except ImportError:
    ImageGrab = None

from . import amiibo
from . import bananza
from . import dlss
from . import fsr4
from . import ini
from . import layout as layouts
from . import update
from .runner import InstallError
from .guilogic import (DKB_STEP_ORDER, DONE, FAILED, GAME_EXE, PENDING, RUNNING, SKIPPED, UI_LANGUAGES, Catalog, Form, Launcher, Model,
                       Process, Settings, amount, default_dest, default_game_language, describe_event, install_state,
                       libraries_failed, parse_line, play_command, previous_version, release_page, run_precompile,
                       update_status, warm_command, windows_ui_language, _run_precompile_proc)

POLL_MS = 100
GLYPH = {PENDING: '\u25cb', RUNNING: '\u25b6', DONE: '\u2714', SKIPPED: '\u2013', FAILED: '\u2716'}
# The in-game menu's Donkey Kong look (runtime/ui/menu.cpp style_dk), drawn here with our own colours only. The red and
# green of the menu are accents and fills; the text variants (RED_TEXT, GREEN_TEXT) are their lighter tints, because the
# originals reach only 3.0:1 and 4.2:1 on the window bark. Every text/background pair below is >= 4.5:1 (docs/installer.md).
BARK, FRAME, BORDER, FIELD, HOVER = '#2a1a0e', '#3b2614', '#6b4a2a', '#1d1109', '#4d331b'
BANANA, CREAM, MUTED = '#f6c52b', '#f3e9d2', '#b8a888'
RED, RED_TEXT, GREEN, GREEN_TEXT = '#c8281e', '#ff8a7a', '#3f8f3a', '#6fcf68'
BRAND = 'DONKEY KONG BANANZA  ·  PC RECOMP'
COLOR = {PENDING: MUTED, RUNNING: BANANA, DONE: GREEN_TEXT, SKIPPED: MUTED, FAILED: RED_TEXT}
PAD = 14


def apply_theme(root, ui_font):
    """A 'dk' ttk theme on top of clam (portable: clam ships with every Tk). Falls back to the platform theme if missing."""
    style = ttk.Style(root)
    try:
        style.theme_create('dk', parent='clam')
        style.theme_use('dk')
    except tk.TclError:
        pass
    root.configure(background=BARK)
    root.option_add('*TCombobox*Listbox.background', FIELD)
    root.option_add('*TCombobox*Listbox.foreground', CREAM)
    root.option_add('*TCombobox*Listbox.selectBackground', BANANA)
    root.option_add('*TCombobox*Listbox.selectForeground', BARK)
    font = (ui_font, 10)
    style.configure('.', background=BARK, foreground=CREAM, font=font, bordercolor=BORDER, lightcolor=BORDER,
                    darkcolor=BORDER, troughcolor=FIELD, focuscolor=BANANA, insertcolor=CREAM)
    style.configure('TButton', background=FRAME, foreground=CREAM, relief='flat', borderwidth=2, padding=(14, 5),
                    bordercolor=BORDER, lightcolor=BORDER, darkcolor=BORDER)
    style.map('TButton', background=[('disabled', BARK), ('pressed', BANANA), ('active', HOVER)],
              foreground=[('disabled', '#9c8a70'), ('pressed', BARK)], bordercolor=[('focus', BANANA)])
    style.configure('Accent.TButton', background=BANANA, foreground=BARK, font=(ui_font, 10, 'bold'),
                    bordercolor=BANANA, lightcolor=BANANA, darkcolor=BANANA)
    style.map('Accent.TButton', background=[('disabled', BARK), ('pressed', CREAM), ('active', '#ffd95a')],
              foreground=[('disabled', '#9c8a70')])
    style.configure('Toolbutton', background=FRAME, foreground=CREAM, relief='flat', borderwidth=2, padding=(8, 2),
                    bordercolor=BORDER, lightcolor=BORDER, darkcolor=BORDER)
    style.map('Toolbutton', background=[('selected', BANANA), ('active', HOVER)], foreground=[('selected', BARK)])
    for name in ('TEntry', 'TCombobox'):
        style.configure(name, fieldbackground=FIELD, foreground=CREAM, insertcolor=CREAM, bordercolor=BORDER,
                        lightcolor=BORDER, darkcolor=BORDER, arrowcolor=BANANA, padding=3)
    style.map('TEntry', bordercolor=[('focus', BANANA)], lightcolor=[('focus', BANANA)], darkcolor=[('focus', BANANA)])
    style.map('TCombobox', fieldbackground=[('readonly', FIELD)], foreground=[('readonly', CREAM)],
              selectbackground=[('readonly', FIELD)], selectforeground=[('readonly', CREAM)],
              bordercolor=[('focus', BANANA)], background=[('active', HOVER)])
    style.configure('Horizontal.TProgressbar', background=BANANA, troughcolor=FIELD, bordercolor=BORDER,
                    lightcolor=BANANA, darkcolor=BANANA, thickness=14)
    style.configure('TScrollbar', background=FRAME, troughcolor=FIELD, arrowcolor=BANANA, bordercolor=BORDER)
    style.configure('TCheckbutton', background=BARK, foreground=CREAM, indicatorbackground=FIELD,
                    indicatorforeground=BANANA, indicatormargin=(0, 0, 8, 0), indicatorsize=14)
    style.map('TCheckbutton', background=[('active', BARK)], indicatorbackground=[('selected', FIELD), ('active', HOVER)])
    style.configure('Header.TFrame', background=FRAME)
    style.configure('Brand.TLabel', background=FRAME, foreground=BANANA, font=(ui_font, 14, 'bold'))
    style.configure('Title.TLabel', foreground=BANANA, font=(ui_font, 15, 'bold'))
    style.configure('Sub.TLabel', background=FRAME, foreground=CREAM, font=(ui_font, 10))
    style.configure('Notice.TLabel', background=FRAME, foreground=CREAM, padding=8, borderwidth=2, relief='solid',
                    bordercolor=BANANA)
    style.configure('Err.TLabel', foreground=RED_TEXT)
    style.configure('Warn.TLabel', foreground=BANANA)
    style.configure('Link.TLabel', foreground=MUTED, font=(ui_font, 9, 'underline'))
    style.configure('Version.TLabel', background=FRAME, foreground=MUTED)
    return style


def draw_bananas(canvas, x, y, s):
    """A small banana bunch: three crescents (polygons between two arcs) fanning from a brown stem at (x, y)."""
    import math
    shades = (('#dea51e', -0.5), (BANANA, 0.5), ('#ffd650', 0.0))   # the middle one is drawn last, on top
    for fill, rot in shades:
        cr, sr = math.cos(rot), math.sin(rot)
        outer, inner = [], []
        for i in range(15):
            t = i / 14
            a = math.pi - t * 1.45
            w = 0.34 * s * (0.2 + 0.8 * math.sin(math.pi * (0.1 + 0.8 * t)))
            px, py = s * 0.95 * (1 + math.cos(a)), s * 0.95 * math.sin(a)
            for pts, k in ((outer, 0.5), (inner, -0.5)):
                ox, oy = px + math.cos(a) * w * k, py + math.sin(a) * w * k
                pts.append((x + ox * cr - oy * sr, y + ox * sr + oy * cr))
        canvas.create_polygon([c for p in outer + inner[::-1] for c in p], fill=fill, outline='#8a5a1a')
        tx, ty = (outer[-1][0] + inner[-1][0]) / 2, (outer[-1][1] + inner[-1][1]) / 2
        canvas.create_oval(tx - 2, ty - 2, tx + 2, ty + 2, fill='#4a2f18', outline='')
    canvas.create_rectangle(x - 4, y - 6, x + 4, y + 2, fill='#5c3a1a', outline='')


def fix_tcl_paths():
    """The embeddable Python keeps Tcl/Tk next to python.exe (python/tcl); point Tcl at it when it is not found."""
    tcl = Path(sys.executable).resolve().parent / 'tcl'
    if tcl.is_dir() and 'TCL_LIBRARY' not in os.environ:
        for lib, var in (('tcl9.0', 'TCL_LIBRARY'), ('tk9.0', 'TK_LIBRARY')):
            if (tcl / lib).is_dir():
                os.environ[var] = str(tcl / lib)


def fix_dpi_awareness():
    """Use physical window coordinates before Tk starts, so its geometry agrees with screenshot coordinates."""
    if os.name != 'nt':
        return
    import ctypes
    set_context = getattr(ctypes.windll.user32, 'SetProcessDpiAwarenessContext', None)
    if set_context:
        set_context.argtypes = [ctypes.c_void_p]
        set_context.restype = ctypes.c_int
        set_context(ctypes.c_void_p(-4))   # PER_MONITOR_AWARE_V2; an already configured process keeps its context


SUPPORT_URL = 'https://ko-fi.com/lucasblippng'   # fixed: never user or remote data, so webbrowser may open it directly


def open_page(url):
    """A release page in the browser. Only a https://github.com/ link (guilogic.release_page): the link comes from
    GitHub's answer, and webbrowser hands anything else, a file path too, to the shell to open."""
    page = release_page(url)
    if page:
        webbrowser.open(page)


class App:
    def __init__(self, root, ui_lang, layout, run_args=None, game='totk', check_updates=False, launcher=None):
        self.root, self.layout = root, layout
        self.game = game
        self.launcher = launcher         # a Launcher when launcher.c started the window, else None (the plain setup)
        self.settings = Settings.load(launcher.base) if launcher else Settings()
        self.offer = None                # the latest release (update.Offer) once the check answered
        self.update_line = (('ui.update_checking' if check_updates else 'ui.update_off'), {})
        self.update_offered = False
        self.update_stop = threading.Event()
        self.precompile_stop = threading.Event()
        self.precompile_running = False
        self.precompile_after = None
        self.precompile_closing = False
        self.precompile_skip = False
        self.precompile_dest = ''
        self.precompile_return_screen = 'form'
        self.precompile_step = ('', {})
        self.precompile_fraction = None
        self.precompile_failure = None
        self.cat = Catalog(ui_lang, game)
        self.model = Model(DKB_STEP_ORDER if game == 'bananza' else None)
        self.process = None
        self.closing = False
        self.run_args = run_args or {}   # extra CLI options for tests: temp, verify
        self.log_entries = []            # events and raw lines of the run, rendered by render_log
        self.bound = []                  # (widget, option, id, params) retranslated on a language switch
        self.vars = {n: tk.StringVar() for n in ('base', 'update', 'keys', 'nup', 'exefs', 'romfs', 'package', 'dest')}
        self.vars['lang'] = tk.StringVar()
        self.fsr4_var = tk.BooleanVar(value=False)   # Bananza: AMD FSR 4 wanted
        self.fsr4_accepted = False                   # the license of fsr4.TAG was accepted in this window
        self.dlss_var = tk.BooleanVar(value=False)   # Bananza: NVIDIA DLSS wanted
        self.dlss_accepted = False                   # the license of dlss.TAG was accepted in this window
        self.in_place_var = tk.BooleanVar(value=False)   # Bananza: read the RomFS folder in place, no packed copy
        self.in_place_touched = False                    # ticked or cleared here: the install's record no longer sets it
        self.lang_code = default_game_language(ui_lang)
        self.screen = None
        self.fitted = None               # the (width, height) fit last gave the window

        root.title(self.cat('ui.title'))
        icon = layout.runtime / 'dkb.ico'
        png = layout.runtime / 'totk.png'   # Tk loads no .ico off Windows; the Linux release converts it
        if os.name == 'nt' and icon.is_file():
            root.iconbitmap(default=str(icon))
        elif png.is_file():
            self.icon = tk.PhotoImage(file=str(png))
            root.iconphoto(True, self.icon)
        root.protocol('WM_DELETE_WINDOW', self.on_close)
        self.ui_font = 'Segoe UI' if os.name == 'nt' else 'DejaVu Sans'
        if self.cat.lang == 'zh' and os.name == 'nt':   # Segoe UI draws Chinese through a fallback Tk cannot
            self.ui_font = 'Microsoft YaHei UI'
        self.mono_font = 'Consolas' if os.name == 'nt' else 'DejaVu Sans Mono'
        apply_theme(root, self.ui_font)

        top = ttk.Frame(root, style='Header.TFrame', padding=(PAD, 10, PAD, 10))
        top.pack(fill='x')
        art = tk.Canvas(top, width=46, height=42, bg=FRAME, highlightthickness=0)
        draw_bananas(art, 12, 16, 17)
        art.pack(side='left', padx=(0, 10))
        names = ttk.Frame(top, style='Header.TFrame')
        names.pack(side='left')
        ttk.Label(names, text=BRAND, style='Brand.TLabel').pack(anchor='w')
        self.title = self.label(names, 'ui.title', style='Sub.TLabel')
        self.title.pack(anchor='w')
        version = ttk.Label(top, style='Version.TLabel')
        self.bind_text(version, 'text', 'ui.version', version=layout.release)
        version.pack(side='left', padx=(10, 0), pady=(6, 0))
        sw = ttk.Frame(top, style='Header.TFrame')
        sw.pack(side='right')
        self.ui_var = tk.StringVar(value=self.cat.lang)
        labels = {'zh': '中文'}   # the codes are Latin; Chinese reads better as itself
        for code in UI_LANGUAGES:
            ttk.Radiobutton(sw, text=labels.get(code, code.upper()), value=code, variable=self.ui_var,
                            style='Toolbutton', command=self.switch_language).pack(side='left', padx=1)
        tk.Frame(root, height=4, bg=RED).pack(fill='x')   # the DK tie's red under the strip
        self.body = ttk.Frame(root, padding=PAD)
        self.body.pack(fill='both', expand=True)
        support = self.label(self.body, 'ui.support', style='Link.TLabel')
        support.configure(cursor='hand2')
        support.bind('<Button-1>', lambda _e: webbrowser.open(SUPPORT_URL))
        support.pack(side='bottom', anchor='e')
        self.amiibo_rows = []                      # (label, folder getter) of each amiibo row, for refresh_amiibo
        self.frames = {'form': self.build_form(), 'run': self.build_run(), 'finish': self.build_finish(),
                       'precompile': self.build_precompile()}
        if launcher:
            self.run_args.setdefault('temp', launcher.temp)
            self.frames.update(home=self.build_home(), update=self.build_update())
        self.vars['lang'].set(self.lang_label(self.lang_code))
        self.vars['dest'].trace_add('write', lambda *_: self.refresh_form())
        if launcher:
            self.vars['dest'].set(self.settings.dest or default_dest(launcher.base))
            state = install_state(self.settings.dest)
            if launcher.after_update and state:   # the new version's first start: bring the folder up to date
                self.root.after(300, self.start)
                self.show('form')
            else:
                self.show_home() if state else self.show('form')
            if check_updates:
                self.check_now()
        else:
            self.show('form')
            if check_updates:
                self.update_label = ttk.Label(self.body, style='Warn.TLabel', cursor='hand2')
                threading.Thread(target=self.check_update, daemon=True).start()

    def later(self, fn, *args):
        """fn(*args) on Tk's thread, from a worker thread; nothing once the window is gone."""
        if self.closing:
            return
        try:
            self.root.after(0, fn, *args)
        except (RuntimeError, tk.TclError):   # Tk is already gone: destroyed, or its main loop has ended
            pass

    def check_update(self):
        """Background lookup of a newer release; the label appears only when there is one (never blocks the window)."""
        found = update.newer_release(self.layout.release)
        if found:
            self.later(self.show_update, *found)

    def show_update(self, tag, url):
        self.bind_text(self.update_label, 'text', 'ui.update_available', tag=tag)
        self.update_label.bind('<Button-1>', lambda _e: open_page(url))
        self.update_label.pack(before=self.body.winfo_children()[0], anchor='w', pady=(0, 8))
        self.fit()

    # ---- text -----------------------------------------------------------------------------------------------------

    def bind_text(self, widget, option, id, **params):
        self.bound.append((widget, option, id, params))
        widget.configure(**{option: self.cat(id, **params)})

    def label(self, parent, id, **kw):
        w = ttk.Label(parent, **kw)
        self.bind_text(w, 'text', id)
        return w

    def button(self, parent, id, command, **kw):
        w = ttk.Button(parent, command=command, **kw)
        self.bind_text(w, 'text', id)
        return w

    def retext(self):
        self.root.title(self.cat('ui.title'))
        for widget, option, id, params in self.bound:
            widget.configure(**{option: self.cat(id, **params)})
        self.fill_languages()
        self.refresh_form()
        self.refresh_run()
        self.refresh_finish()
        self.refresh_precompile()
        if self.launcher:
            self.refresh_home()
            self.refresh_update()
        self.render_log()

    def switch_language(self):
        self.cat.set_language(self.ui_var.get())
        code = self.selected_lang()
        self.retext()
        self.vars['lang'].set(self.lang_label(code))
        self.fit()

    def lang_label(self, code):
        return self.cat('lang.' + code)

    def fill_languages(self):
        code = self.selected_lang()
        self.lang_box.configure(values=[self.lang_label(c) for c in ini.LANGUAGES])
        self.vars['lang'].set(self.lang_label(code))

    def selected_lang(self):
        """The game language code behind the combobox text (kept when it is not one of the labels)."""
        shown = self.vars['lang'].get()
        for code in ini.LANGUAGES:
            for lang in UI_LANGUAGES:
                if shown == Catalog(lang)('lang.' + code):
                    return code
        return self.lang_code

    # ---- screens --------------------------------------------------------------------------------------------------

    def show(self, name):
        for f in self.frames.values():
            f.pack_forget()
        self.frames[name].pack(fill='both', expand=True)
        self.screen = name
        self.fit()

    def fit(self):
        """Size the window to its largest screen (an unpacked frame still reports the size its children ask for), so
        a screen or language switch neither cuts text nor moves the buttons. The window only grows: opening the log
        makes it taller, and a size the user dragged it to stays. Every text change that can add lines calls it again,
        or pack takes the missing height from the button bars."""
        self.root.update_idletasks()
        sizes = [(f.winfo_reqwidth(), f.winfo_reqheight()) for f in self.frames.values()]
        chrome = self.root.winfo_reqheight() - self.body.winfo_reqheight()   # the title row above the body
        w = max(self.root.winfo_reqwidth(), max(s[0] for s in sizes) + 2 * PAD)
        h = chrome + max(s[1] for s in sizes) + 2 * PAD
        if (w, h) == self.fitted:   # refresh_run calls it on every poll: leave the window alone while nothing grew
            return
        self.fitted = (w, h)
        self.root.minsize(w, h)
        self.root.geometry(f'{max(w, self.root.winfo_width())}x{max(h, self.root.winfo_height())}')

    def notice(self, parent):
        w = ttk.Label(parent, style='Notice.TLabel', wraplength=620, justify='left')
        self.bind_text(w, 'text', 'ui.notice')
        return w

    def build_form(self):
        f = ttk.Frame(self.body)
        bar = ttk.Frame(f)   # packed first so pack shrinks the text above it, never the buttons
        bar.pack(side='bottom', fill='x', pady=(12, 0))
        self.start_button = self.button(bar, 'ui.start', self.start, style='Accent.TButton')
        self.start_button.pack(side='right')
        self.form_intro = self.label(f, 'ui.intro', wraplength=620, justify='left')
        self.form_intro.pack(anchor='w', pady=(0, 8))
        self.notice(f).pack(fill='x', pady=(0, 10))
        grid = ttk.Frame(f)
        grid.pack(fill='x')
        grid.columnconfigure(1, weight=1)
        rows = (('base', 'ui.base', 'ui.pick_game'), ('update', 'ui.update', 'ui.pick_update'),
                ('keys', 'ui.keys', 'ui.pick_keys'))
        if self.game == 'bananza':
            rows = (('nup', 'dkb.ui.nup', 'dkb.ui.pick_nup'), ('exefs', 'dkb.ui.exefs', 'dkb.ui.pick_exefs'),
                    ('romfs', 'dkb.ui.romfs', 'dkb.ui.pick_romfs'), ('package', 'dkb.ui.package', 'dkb.ui.pick_package'))
        self.form_rows = {}
        for i, (name, label_id, pick_id) in enumerate(rows):
            lab = self.label(grid, label_id)
            lab.grid(row=i, column=0, sticky='w', pady=4, padx=(0, 10))
            ttk.Entry(grid, textvariable=self.vars[name]).grid(row=i, column=1, sticky='ew', pady=4)
            b = self.button(grid, 'ui.browse', lambda n=name, p=pick_id: self.pick_file(n, p))
            b.grid(row=i, column=2, padx=(8, 0))
            self.form_rows[name] = lab
        i = len(rows)
        self.label(grid, 'ui.game_language').grid(row=i, column=0, sticky='w', pady=4)
        self.lang_box = ttk.Combobox(grid, textvariable=self.vars['lang'], state='readonly', width=30)
        self.lang_box.grid(row=i, column=1, sticky='w', pady=4)
        self.lang_touched = False
        self.lang_box.bind('<<ComboboxSelected>>', lambda _e: setattr(self, 'lang_touched', True))
        self.fill_languages()
        self.label(grid, 'ui.dest').grid(row=i + 1, column=0, sticky='w', pady=4)
        ttk.Entry(grid, textvariable=self.vars['dest']).grid(row=i + 1, column=1, sticky='ew', pady=4)
        self.button(grid, 'ui.browse', self.pick_dest).grid(row=i + 1, column=2, padx=(8, 0))
        if self.game == 'bananza':
            box = ttk.Checkbutton(f, variable=self.in_place_var,
                                  command=lambda: setattr(self, 'in_place_touched', True))
            self.bind_text(box, 'text', 'dkb.ui.romfs_in_place')
            box.pack(anchor='w', pady=(8, 0))
        if self.game == 'bananza' and os.name == 'nt':   # AMD's libraries are for Windows only
            self.fsr4_box = ttk.Checkbutton(f, variable=self.fsr4_var, command=self.toggle_fsr4)
            self.bind_text(self.fsr4_box, 'text', 'dkb.ui.fsr4')
            self.fsr4_box.pack(anchor='w', pady=(8, 0))
            self.dlss_box = ttk.Checkbutton(f, variable=self.dlss_var, command=self.toggle_dlss)
            self.bind_text(self.dlss_box, 'text', 'dkb.ui.dlss')
            self.dlss_box.pack(anchor='w', pady=(8, 0))
        self.update_hint = ttk.Label(f, wraplength=620, justify='left')
        self.update_hint.pack(anchor='w', pady=(8, 0))
        # Shown once the install folder holds an installation (refresh_form): the setup window opened by the Linux
        # AppImage or the setup zip has no home screen, so the form is where the player finds it.
        self.form_amiibo = None
        if self.game == 'bananza':
            self.form_amiibo = self.amiibo_row(f, lambda: self.vars['dest'].get().strip())
        self.form_error = ttk.Label(f, style='Err.TLabel', wraplength=620, justify='left')
        self.form_error.pack(anchor='w', pady=(8, 0))
        if self.game != 'bananza':
            self.label(f, 'ui.keys_private', wraplength=620, justify='left').pack(anchor='w', pady=(8, 0))
        return f

    def build_run(self):
        f = ttk.Frame(self.body)
        bar = ttk.Frame(f)
        bar.pack(side='bottom', fill='x', pady=(8, 0))
        self.cancel_button = self.button(bar, 'ui.cancel', self.cancel)
        self.cancel_button.pack(side='right')
        self.log_toggle = self.button(bar, 'ui.show_log', self.toggle_log)
        self.log_toggle.pack(side='left')
        self.run_header = ttk.Label(f, font=(self.ui_font, 11, 'bold'))
        self.run_header.pack(anchor='w')
        self.step_rows = {}
        self.steps_box = ttk.Frame(f)
        self.steps_box.pack(fill='x', pady=(8, 8))
        self.phase_label = ttk.Label(f, wraplength=620, justify='left')
        self.phase_label.pack(anchor='w')
        self.bar = ttk.Progressbar(f, mode='determinate', maximum=1000)
        self.bar.pack(fill='x', pady=(4, 6))
        self.space_label = ttk.Label(f, wraplength=620, justify='left')
        self.space_label.pack(anchor='w')
        self.warn_label = ttk.Label(f, wraplength=620, justify='left', style='Warn.TLabel')
        self.warn_label.pack(anchor='w')
        self.log_frame = ttk.Frame(f)
        self.log = tk.Text(self.log_frame, height=10, wrap='word', state='disabled', font=(self.mono_font, 9), bg=FIELD, fg=CREAM, insertbackground=CREAM,
                            relief='flat', highlightthickness=2, highlightbackground=BORDER, highlightcolor=BORDER)
        scroll = ttk.Scrollbar(self.log_frame, command=self.log.yview)
        self.log.configure(yscrollcommand=scroll.set)
        scroll.pack(side='right', fill='y')
        self.log.pack(side='left', fill='both', expand=True)
        self.log_open = False
        return f

    def build_finish(self):
        f = ttk.Frame(self.body)
        bar = ttk.Frame(f)
        bar.pack(side='bottom', fill='x', pady=(12, 0))
        self.button(bar, 'ui.close', self.on_close).pack(side='right')
        self.open_button = self.button(bar, 'ui.open_folder', self.open_folder)
        self.open_button.pack(side='right', padx=(0, 8))
        self.back_button = self.button(bar, 'ui.back', self.back)
        self.back_button.pack(side='left')
        self.finish_title = ttk.Label(f, style='Title.TLabel', wraplength=620, justify='left')
        self.finish_title.pack(anchor='w')
        self.finish_text = ttk.Label(f, wraplength=620, justify='left')
        self.finish_text.pack(anchor='w', pady=(8, 8))
        self.finish_steps = ttk.Frame(f)
        self.finish_steps.pack(fill='x', pady=(0, 8))
        self.finish_notice = self.notice(f)
        self.finish_notice.pack(fill='x', pady=(8, 0))
        self.finish_build = ttk.Frame(f)
        self.finish_build_text = ttk.Label(self.finish_build, wraplength=520, justify='left')
        self.finish_build_text.pack(side='left')
        self.button(self.finish_build, 'ui.delete_build', self.delete_build).pack(side='right')
        self.uninstall_button = self.button(bar, 'ui.uninstall', self.uninstall)
        self.play_after = self.button(bar, 'ui.play', lambda: self.play(self.model.dest), style='Accent.TButton')
        return f

    # ---- the launcher's home and update screens -----------------------------------------------------------------

    def build_home(self):
        f = ttk.Frame(self.body)
        bar = ttk.Frame(f)
        bar.pack(side='bottom', fill='x', pady=(12, 0))
        self.play_button = self.button(bar, 'ui.play', lambda: self.play(self.settings.dest), style='Accent.TButton')
        self.play_button.pack(side='right')
        self.button(bar, 'ui.open_folder', lambda: self.open_path(self.settings.dest)).pack(side='right', padx=(0, 8))
        self.button(bar, 'ui.setup_again', lambda: self.show('form')).pack(side='left')
        self.home_title = ttk.Label(f, style='Title.TLabel', wraplength=620, justify='left')
        self.home_title.pack(anchor='w')
        self.home_text = ttk.Label(f, wraplength=620, justify='left')
        self.home_text.pack(anchor='w', pady=(8, 0))
        self.home_tools = ttk.Label(f, wraplength=620, justify='left', foreground=MUTED)
        self.home_tools.pack(anchor='w', pady=(4, 0))
        if self.game == 'bananza':
            self.amiibo_row(f, lambda: self.settings.dest).pack(fill='x', pady=(10, 0))
        self.update_box = ttk.Frame(f)
        self.update_box.pack(fill='both', expand=True, pady=(14, 0))
        self.update_text = ttk.Label(self.update_box, wraplength=620, justify='left', style='Warn.TLabel')
        self.update_text.pack(anchor='w')
        self.notes = tk.Text(self.update_box, height=7, width=80, wrap='word', state='disabled', font=(self.ui_font, 9),
                             bg=FIELD, fg=CREAM, relief='flat', highlightthickness=2, highlightbackground=BORDER,
                             highlightcolor=BORDER)
        self.check_var = tk.BooleanVar(value=self.settings.check_updates)
        check = ttk.Checkbutton(self.update_box, variable=self.check_var, command=self.toggle_check)
        self.bind_text(check, 'text', 'ui.check_updates')
        check.pack(side='bottom', anchor='w', pady=(8, 0))
        row = ttk.Frame(self.update_box)
        row.pack(side='bottom', fill='x', pady=(8, 0))
        self.check_button = self.button(row, 'ui.check_now', self.check_now)
        self.check_button.pack(side='left')
        self.page_button = self.button(row, 'ui.release_page', lambda: self.offer and open_page(self.offer.page))
        self.update_button = ttk.Button(row, command=self.start_update, style='Accent.TButton')
        self.go_back_button = ttk.Button(row, command=self.go_back)
        return f

    def build_update(self):
        f = ttk.Frame(self.body)
        bar = ttk.Frame(f)
        bar.pack(side='bottom', fill='x', pady=(12, 0))
        self.update_cancel = self.button(bar, 'ui.cancel', self.update_stop.set)
        self.update_cancel.pack(side='right')
        self.update_back = self.button(bar, 'ui.back', self.show_home)
        self.update_title = ttk.Label(f, style='Title.TLabel', wraplength=620, justify='left')
        self.update_title.pack(anchor='w')
        self.update_phase = ttk.Label(f, wraplength=620, justify='left')
        self.update_phase.pack(anchor='w', pady=(8, 4))
        self.update_bar = ttk.Progressbar(f, mode='determinate', maximum=1000)
        self.update_bar.pack(fill='x', pady=(4, 6))
        self.update_error = ttk.Label(f, style='Err.TLabel', wraplength=620, justify='left')
        self.update_error.pack(anchor='w', pady=(8, 0))
        self.update_step = ('', {})
        self.update_failure = None
        self.update_target = ('ui.updating', '')   # the screen's title and the tag it switches to
        self.update_running = False                # a worker thread downloads, unpacks or hands over
        self.update_closing = False                # the window was closed during it: it closes when the worker ends
        return f

    def build_precompile(self):
        f = ttk.Frame(self.body)
        bar = ttk.Frame(f)
        bar.pack(side='bottom', fill='x', pady=(12, 0))
        self.precompile_cancel = self.button(bar, 'ui.cancel', self.cancel_precompile)
        self.precompile_cancel.pack(side='right')
        self.precompile_skip_btn = self.button(bar, 'ui.skip_and_play', self.skip_precompile)
        self.precompile_back = self.button(bar, 'ui.back', self.back_from_precompile)
        self.precompile_play_anyway = self.button(bar, 'ui.play_anyway', lambda: self.launch_game(self.precompile_dest),
                                                  style='Accent.TButton')
        self.precompile_title = self.label(f, 'ui.precompile_title', style='Title.TLabel', wraplength=620, justify='left')
        self.precompile_title.pack(anchor='w')
        self.precompile_note = self.label(f, 'ui.precompile_note', wraplength=620, justify='left')
        self.precompile_note.pack(anchor='w', pady=(6, 4))
        self.precompile_phase = ttk.Label(f, wraplength=620, justify='left')
        self.precompile_phase.pack(anchor='w', pady=(8, 4))
        self.precompile_bar = ttk.Progressbar(f, mode='indeterminate', maximum=1000)
        self.precompile_bar.pack(fill='x', pady=(4, 6))
        self.precompile_error = ttk.Label(f, style='Err.TLabel', wraplength=620, justify='left')
        self.precompile_error.pack(anchor='w', pady=(8, 0))
        return f

    def refresh_precompile(self):
        sid, params = self.precompile_step
        self.precompile_phase.configure(text=self.cat(sid, **params) if sid else '')
        if self.precompile_failure:
            self.precompile_error.configure(text=self.cat('ui.precompile_failed', detail=self.precompile_failure))
        else:
            self.precompile_error.configure(text='')
        frac = self.precompile_fraction
        if self.precompile_running and frac is None:
            if str(self.precompile_bar.cget('mode')) != 'indeterminate':
                self.precompile_bar.configure(mode='indeterminate')
                self.precompile_bar.start(15)
        else:
            if str(self.precompile_bar.cget('mode')) == 'indeterminate':
                self.precompile_bar.stop()
                self.precompile_bar.configure(mode='determinate')
            self.precompile_bar.configure(value=(frac or 0.0) * 1000)
        for btn in (self.precompile_cancel, self.precompile_skip_btn, self.precompile_back, self.precompile_play_anyway):
            btn.pack_forget()
        if self.precompile_running:
            self.precompile_cancel.pack(side='right')
            self.precompile_cancel.state(['!disabled'])
            self.precompile_skip_btn.pack(side='right', padx=(0, 8))
            self.precompile_skip_btn.state(['!disabled'])
        elif self.precompile_failure:
            self.precompile_back.pack(side='left')
            self.precompile_play_anyway.pack(side='right')
        self.fit()

    def show_precompile_if_busy(self):
        if self.precompile_running and self.screen != 'precompile':
            self.refresh_precompile()
            self.show('precompile')

    def enter_precompile_phase(self, phase):
        if not self.precompile_running:
            return
        self.precompile_step = (f'ui.precompile_{phase}', {})
        self.precompile_fraction = None
        self.show_precompile_if_busy()
        self.refresh_precompile()

    def update_precompile_progress(self, done, total):
        if not self.precompile_running:
            return
        if done is not None and total:
            self.precompile_step = ('ui.precompile_warm_count', {'done': done, 'total': total})
            self.precompile_fraction = max(0.0, min(1.0, done / total))
        else:
            self.precompile_fraction = None
        self.refresh_precompile()

    def cancel_precompile(self):
        if not self.precompile_running:
            return
        self.precompile_cancel.state(['disabled'])
        self.precompile_skip_btn.state(['disabled'])
        self.precompile_stop.set()

    def skip_precompile(self):
        if not self.precompile_running:
            return
        self.precompile_skip = True
        self.precompile_cancel.state(['disabled'])
        self.precompile_skip_btn.state(['disabled'])
        self.precompile_stop.set()

    def precompile_failed(self, detail):
        self.precompile_running = False
        self.precompile_failure = detail or 'unknown error'
        self.set_play_enabled(True)
        self.refresh_precompile()
        self.show('precompile')

    def back_from_precompile(self):
        self.precompile_failure = None
        self.precompile_bar.stop()
        if self.launcher and self.precompile_return_screen == 'home' and install_state(self.settings.dest):
            self.show_home()
        else:
            target = self.precompile_return_screen if self.precompile_return_screen in self.frames else 'form'
            self.show(target)

    def set_play_enabled(self, enabled):
        flag = ['!disabled'] if enabled else ['disabled']
        if hasattr(self, 'play_button'):
            self.play_button.state(flag)
        if hasattr(self, 'play_after'):
            self.play_after.state(flag)

    def finish_precompile(self, status, detail):
        self.precompile_running = False
        if self.precompile_after:
            self.root.after_cancel(self.precompile_after)
            self.precompile_after = None
        self.precompile_bar.stop()
        self.set_play_enabled(True)
        if self.precompile_closing:
            self.root.destroy()
            return
        if self.precompile_skip or status in ('ready', 'ok'):
            self.launch_game(self.precompile_dest)
            return
        if status == 'cancelled':
            self.back_from_precompile()
            return
        self.precompile_failed(detail)

    def show_home(self):
        self.refresh_home()
        self.show('home')

    def refresh_home(self):
        if not self.launcher:
            return
        state = install_state(self.settings.dest)
        self.home_title.configure(text=self.cat('ui.home_ready' if state == 'ready' else 'ui.home_unfinished'))
        self.home_text.configure(text=self.cat('ui.home_text', dest=self.settings.dest))
        self.refresh_amiibo()
        self.home_tools.configure(text=self.cat('ui.home_tools', version=self.layout.release, base=self.launcher.base))
        self.play_button.state(['!disabled' if state == 'ready' else 'disabled'])
        id, params = self.update_line
        self.update_text.configure(text=self.cat(id, **params))
        notes = (self.offer.notes or '').strip() if self.offer and self.offer.direction > 0 else ''
        self.notes.configure(state='normal')
        self.notes.delete('1.0', 'end')
        self.notes.insert('1.0', notes)   # the release's own text, shown as plain text
        self.notes.configure(state='disabled')
        if notes:
            self.notes.pack(fill='x', pady=(6, 0), after=self.update_text)
        else:
            self.notes.pack_forget()
        for button, show in ((self.update_button, self.update_offered), (self.page_button, bool(self.offer and self.offer.page and self.offer.direction > 0))):
            if show:
                button.pack(side='right', padx=(0, 8))
            else:
                button.pack_forget()
        if self.offer:
            self.update_button.configure(text=self.cat('ui.update_now', tag=self.offer.tag))
        back = self.previous_tag()
        if back:
            self.go_back_button.configure(text=self.cat('ui.go_back', tag=back))
            self.go_back_button.pack(side='right', padx=(0, 8))
        else:
            self.go_back_button.pack_forget()
        self.fit()

    def previous_tag(self):
        """The version the home screen offers to go back to (guilogic.previous_version), or None."""
        return previous_version(self.launcher.base, update.own_tag(self.launcher.base, self.layout.root))

    def toggle_check(self):
        self.settings.check_updates = bool(self.check_var.get())
        self.settings.save(self.launcher.base)
        self.launcher.note(f'check at start: {"on" if self.settings.check_updates else "off"}')

    def check_now(self):
        """The release check in the background: the home screen says what it found, and never waits for it."""
        self.update_line, self.offer, self.update_offered = ('ui.update_checking', {}), None, False
        self.check_button.state(['disabled'])
        self.refresh_home()

        def work():
            offer = error = None
            try:
                offer = update.check(self.layout.release)
                self.launcher.note(f'check: latest {offer.tag} ({offer.version}), this {self.layout.release}, '
                                   f'direction {offer.direction}, manifest {"yes" if offer.manifest else "no"}')
            except update.UpdateError as e:   # offline: the reason alone; a release that disagrees with itself: why
                error = e.params.get('detail', '') if e.id == 'update.offline' else self.cat(e.id, **e.params)
                self.launcher.note(f'check: {e.id} {e.params}')
            self.later(self.show_offer, offer, error)
        threading.Thread(target=work, daemon=True).start()

    def show_offer(self, offer, error):
        self.offer = offer
        self.update_line, self.update_offered = update_status(offer, error, self.launcher.allow_downgrade)
        self.check_button.state(['!disabled'])
        self.refresh_home()

    def start_update(self):
        """Download, check and unpack the offered release in the background, then hand over to its window."""
        offer = self.offer
        if not self.update_offered or offer is None or self.process or self.update_running:
            return
        self.begin_update('ui.updating', offer.tag,
                          ('ui.update_downloading', {'done': 0, 'total': -(-offer.manifest.tools.size >> 20)}))
        self.launcher.note(f'update to {offer.tag}: {offer.manifest.tools.name}, {offer.manifest.tools.size} bytes, '
                           f'sha256 {offer.manifest.tools.sha256}')
        threading.Thread(target=self.update_worker, args=(offer,), daemon=True).start()

    def go_back(self):
        """Start the previous version again in place of this one: its window confirms its start like an update's
        (update.confirm_start makes it current.txt and this one previous.txt) and brings the game folder back to it.
        Nothing is downloaded; this version stays, so the next check offers it again."""
        tag = self.previous_tag()
        if not tag or self.process or self.update_running:
            return
        if not messagebox.askyesno(self.cat('ui.title'), self.cat('ui.go_back_confirm', tag=tag, current=self.layout.release)):
            return
        self.begin_update('ui.going_back', tag, ('ui.update_starting', {'tag': tag}))
        self.update_progress(self.update_step, None)
        self.launcher.note(f'go back to {tag} from {self.layout.release}')
        threading.Thread(target=self.update_worker_body, args=(f'go back to {tag}', lambda: self.hand_over(tag)),
                         daemon=True).start()

    def begin_update(self, title_id, tag, step):
        self.update_stop.clear()
        self.update_running, self.update_closing = True, False
        self.update_target, self.update_step, self.update_failure = (title_id, tag), step, None
        self.update_back.pack_forget()
        self.update_cancel.pack(side='right')
        self.update_cancel.state(['!disabled'])
        self.update_bar.configure(mode='determinate', value=0)
        self.refresh_update()
        self.show('update')

    def update_worker(self, offer):
        base, tools = self.launcher.base, offer.manifest.tools
        total = -(-tools.size >> 20)
        shown = [0.0]

        def progress(done, size):
            now = time.monotonic()
            if now - shown[0] > 0.2 or done == size:
                shown[0] = now
                self.later(self.update_progress, ('ui.update_downloading', {'done': done >> 20, 'total': total}), done / size)

        def work():
            zip_path = update.download(tools, offer.tag, base / 'downloads', progress=progress,
                                       cancelled=self.update_stop.is_set)
            self.launcher.note(f'update to {offer.tag}: downloaded and checked {zip_path.name}')
            if self.update_stop.is_set():   # Cancel after the last block came in: the checked zip is not unpacked
                zip_path.unlink(missing_ok=True)
                raise update.UpdateError('update.cancelled')
            self.later(self.update_progress, ('ui.update_unpacking', {}), None)
            folder = update.stage(zip_path, base, offer.manifest)
            zip_path.unlink(missing_ok=True)
            self.launcher.note(f'update to {offer.tag}: unpacked into {folder}')
            if self.update_closing:   # closed while it unpacked: the folder stays for the next try, nothing starts
                return
            if self.update_stop.is_set():   # Cancel before its button went grey: the folder stays, nothing starts
                raise update.UpdateError('update.cancelled')
            self.hand_over(offer.tag)
        self.update_worker_body(f'update to {offer.tag}', work)

    def hand_over(self, tag):
        """tag's window started with --after-update; this window closes once it confirms, or says why it did not"""
        base = self.launcher.base
        self.later(self.update_progress, ('ui.update_starting', {'tag': tag}), None)
        result = update.start_version(base, tag, self.launcher.exe, ('--lang', self.cat.lang))
        self.launcher.note(f'{tag}: its window {result}')
        if result == 'failed':
            current = update.read_pointer(base, 'current') or self.layout.release
            self.later(self.update_failed, ('ui.update_start_failed', {'tag': tag, 'current': current,
                                                                       'log': str(self.launcher.log)}))
        else:
            self.update_closing = True

    def update_worker_body(self, what, work):
        """work() on the worker thread, its failure shown on the update screen; the window closes afterwards when it
        was closed meanwhile or the new version's window took over"""
        try:
            work()
        except update.UpdateError as e:
            self.launcher.note(f'{what}: {e.id} {e.params}')
            if e.id != 'update.cancelled' or not self.update_closing:
                self.later(self.update_failed, (e.id, e.params))
        except OSError as e:
            self.launcher.note(f'{what}: {type(e).__name__} {e}')
            self.later(self.update_failed, ('update.stage_failed', {'path': str(self.launcher.base), 'detail': str(e)}))
        finally:
            self.update_running = False
            if self.update_closing:
                self.later(self.close_after_update)

    def close_after_update(self):
        self.update_bar.stop()   # its animation would fire once more into a destroyed window
        self.root.destroy()

    def update_progress(self, step, fraction):
        self.update_step = step
        if fraction is None:
            if str(self.update_bar.cget('mode')) != 'indeterminate':
                self.update_bar.configure(mode='indeterminate')
                self.update_bar.start(15)
            self.update_cancel.state(['disabled'])   # unpacking and starting are not cut half way
        else:
            self.update_bar.configure(value=fraction * 1000)
        self.refresh_update()

    def update_failed(self, failure):
        self.update_failure = failure
        if str(self.update_bar.cget('mode')) == 'indeterminate':
            self.update_bar.stop()
        self.update_bar.configure(mode='determinate', value=0)
        self.update_cancel.pack_forget()
        self.update_back.pack(side='left')
        self.refresh_update()

    def refresh_update(self):
        title_id, tag = self.update_target
        if not self.launcher or not tag:
            return
        self.update_title.configure(text=self.cat(title_id, tag=tag))
        id, params = self.update_step
        self.update_phase.configure(text=self.cat(id, **params) if id else '')
        if self.update_failure:
            fid, fparams = self.update_failure
            self.update_error.configure(text=self.cat(fid, **fparams) + '\n\n' + self.cat('ui.update_failed'))
        else:
            self.update_error.configure(text='')
        self.fit()

    def launch_game(self, dest):
        """The built game, started from its folder; the launcher closes (an update needs the game closed)."""
        if not dest or install_state(dest) != 'ready':
            return
        kw = {'creationflags': subprocess.CREATE_NEW_PROCESS_GROUP} if os.name == 'nt' else {'start_new_session': True}
        try:
            subprocess.Popen(play_command(dest), cwd=dest, stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                             stderr=subprocess.DEVNULL, **kw)
        except OSError as e:
            messagebox.showerror(self.cat('ui.title'), f'{play_command(dest)[0]}: {e}')
            return
        self.root.destroy()

    # ---- amiibo files (Bananza) ----------------------------------------------------------------------------------

    def amiibo_row(self, parent, dest):
        """A row for the player's own amiibo tag files (installer/amiibo.py): how many the game folder holds, and
        buttons that copy more in or open the folder. dest() gives the game folder at the time of the click; the caller
        packs the row."""
        row = ttk.Frame(parent)
        text = ttk.Label(row, wraplength=300, justify='left')
        text.pack(side='left')
        self.button(row, 'dkb.ui.amiibo_open', lambda: self.open_amiibo(dest())).pack(side='right')
        adders = (('dkb.ui.amiibo_add_folder', self.add_amiibo_folder),
                  ('dkb.ui.amiibo_add_files', self.add_amiibo_files))
        for id, act in adders:
            self.button(row, id, lambda act=act: act(dest())).pack(side='right', padx=(0, 8))
        self.amiibo_rows.append((text, dest))
        return row

    def refresh_amiibo(self):
        for text, dest in self.amiibo_rows:
            if dest():
                text.configure(text=self.cat('dkb.ui.amiibo_count', n=amiibo.count(dest())))

    def amiibo_done(self, dest, copied, skipped):
        self.refresh_amiibo()
        id = 'dkb.ui.amiibo_added' if copied or not skipped else 'dkb.ui.amiibo_none'
        messagebox.showinfo(self.cat('ui.title'), self.cat(id, n=copied, skipped=skipped, folder=amiibo.folder(dest)))

    def add_amiibo_files(self, dest):
        types = [(self.cat('dkb.ui.type_amiibo'), '*.bin *.nfc'), (self.cat('ui.type_all'), '*.*')]
        paths = filedialog.askopenfilenames(title=self.cat('dkb.ui.amiibo_add_files'), filetypes=types)
        if paths and dest:
            self.amiibo_done(dest, *amiibo.add_files(dest, paths))

    def add_amiibo_folder(self, dest):
        path = filedialog.askdirectory(title=self.cat('dkb.ui.amiibo_add_folder'), mustexist=True)
        if path and dest:
            self.amiibo_done(dest, *amiibo.add_folder(dest, path))

    def open_amiibo(self, dest):
        if dest:
            amiibo.folder(dest).mkdir(parents=True, exist_ok=True)
            self.open_path(str(amiibo.folder(dest)))
    def play(self, dest):
        """Precompile missing shader pipelines if needed, then start the built game."""
        if not dest or install_state(dest) != 'ready' or self.process or getattr(self, 'update_running', False) or self.precompile_running:
            return
        if self.game != 'bananza':
            self.launch_game(dest)
            return
        self.precompile_stop.clear()
        self.precompile_running = True
        self.precompile_closing = False
        self.precompile_skip = False
        self.precompile_dest = dest
        self.precompile_return_screen = self.screen or ('home' if self.launcher else 'finish')
        self.precompile_step = ('ui.precompile_checking', {})
        self.precompile_fraction = None
        self.precompile_failure = None
        self.set_play_enabled(False)
        self.precompile_after = self.root.after(500, self.show_precompile_if_busy)

        def work():
            status, detail = run_precompile(
                dest, layout=self.layout,
                on_phase=lambda phase: self.later(self.enter_precompile_phase, phase),
                on_progress=lambda done, total: self.later(self.update_precompile_progress, done, total),
                cancelled=self.precompile_stop)
            self.later(self.finish_precompile, status, detail)

        threading.Thread(target=work, daemon=True).start()

    def open_path(self, dest):
        if dest and Path(dest).is_dir():
            if os.name == 'nt':
                os.startfile(dest)
            else:
                subprocess.Popen(['xdg-open', dest], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                                 start_new_session=True)

    # ---- form -----------------------------------------------------------------------------------------------------

    def form(self):
        return Form(base=self.vars['base'].get(), update=self.vars['update'].get(), keys=self.vars['keys'].get(),
                    lang=self.selected_lang(), dest=self.vars['dest'].get(), game=self.game,
                    exefs=self.vars['exefs'].get(), romfs=self.vars['romfs'].get(), package=self.vars['package'].get(),
                    nup=self.vars['nup'].get(), fsr4=self.fsr4_choice(), dlss=self.dlss_choice(), romfs_in_place=self.in_place_var.get())

    # AMD FSR 4 (installer/fsr4.py). Ticking the box shows AMD's license of the pinned release, fetched from AMD's
    # repository; only "I accept" leaves it ticked. A folder whose setup recorded that release starts ticked.
    def fsr4_recorded(self):
        state = Form(dest=self.vars['dest'].get(), game=self.game).installed_state()
        return bool(state) and state.get('fsr4') == fsr4.TAG

    def fsr4_choice(self):
        if self.game != 'bananza' or os.name != 'nt':
            return ''
        if self.fsr4_var.get():
            return 'accept' if self.fsr4_accepted else ''
        return 'off' if self.fsr4_recorded() else ''

    def dlss_recorded(self):
        state = Form(dest=self.vars['dest'].get(), game=self.game).installed_state()
        return bool(state) and state.get('dlss') == dlss.TAG

    def dlss_choice(self):   # as fsr4_choice, for NVIDIA DLSS
        if self.game != 'bananza' or os.name != 'nt':
            return ''
        if self.dlss_var.get():
            return 'accept' if self.dlss_accepted else ''
        return 'off' if self.dlss_recorded() else ''

    def toggle_fsr4(self):
        if not self.fsr4_var.get() or self.fsr4_accepted or self.fsr4_recorded():
            return
        self.fsr4_var.set(False)   # ticked only by "I accept"
        self.license_window(fsr4, 'fsr4', self.fsr4_var, 'fsr4_accepted')

    def toggle_dlss(self):
        if not self.dlss_var.get() or self.dlss_accepted or self.dlss_recorded():
            return
        self.dlss_var.set(False)
        self.license_window(dlss, 'dlss', self.dlss_var, 'dlss_accepted')

    def license_window(self, mod, key, var, accepted_attr):
        """Shows the license of mod.TAG (mod.license_text) and ticks var, setting accepted_attr, only on "I accept"."""
        win = tk.Toplevel(self.root)
        win.title(self.cat(f'dkb.ui.{key}_title'))
        win.configure(bg=BARK)
        win.transient(self.root)
        win.grab_set()
        bar = ttk.Frame(win)
        bar.pack(side='bottom', fill='x', padx=12, pady=12)
        accept = ttk.Button(bar, text=self.cat(f'dkb.ui.{key}_accept'), style='Accent.TButton', state='disabled')
        accept.pack(side='right')
        ttk.Button(bar, text=self.cat(f'dkb.ui.{key}_decline'), command=win.destroy).pack(side='right', padx=(0, 8))
        ttk.Label(win, text=self.cat(f'dkb.ui.{key}_intro', release=mod.TAG), wraplength=640,
                  justify='left').pack(anchor='w', padx=12, pady=(12, 8))
        box = ttk.Frame(win)
        box.pack(fill='both', expand=True, padx=12)
        text = tk.Text(box, height=22, width=90, wrap='word', font=(self.mono_font, 9), bg=FIELD, fg=CREAM,
                       relief='flat', highlightthickness=2, highlightbackground=BORDER, highlightcolor=BORDER)
        scroll = ttk.Scrollbar(box, command=text.yview)
        text.configure(yscrollcommand=scroll.set)
        scroll.pack(side='right', fill='y')
        text.pack(side='left', fill='both', expand=True)
        text.insert('end', self.cat(f'dkb.ui.{key}_loading'))
        text.configure(state='disabled')
        result = {}

        def fetch():   # a worker thread: Tk is touched only from show(), which waits for result
            try:
                result['text'] = mod.license_text()
            except mod.FetchError as error:
                result['error'] = error.args[1]
            except Exception as error:   # anything else must not leave the window loading with Accept disabled
                result['error'] = f'{type(error).__name__}: {error}'

        def show():
            if not win.winfo_exists():
                return
            if not result:
                win.after(100, show)
                return
            text.configure(state='normal')
            text.delete('1.0', 'end')
            if 'text' in result:
                text.insert('end', result['text'])
                accept.configure(state='normal')
            else:
                text.insert('end', self.cat(f'dkb.ui.{key}_fetch_failed', detail=result['error'], url=mod.PAGE))
            text.configure(state='disabled')

        def accepted():
            setattr(self, accepted_attr, True)
            var.set(True)
            win.destroy()

        accept.configure(command=accepted)
        threading.Thread(target=fetch, daemon=True).start()
        win.after(100, show)

    def refresh_form(self):
        form = self.form()
        state = form.installed_state()
        if self.game == 'bananza' and not self.fsr4_accepted:
            self.fsr4_var.set(bool(state) and state.get('fsr4') == fsr4.TAG)
        if self.game == 'bananza' and not self.dlss_accepted:
            self.dlss_var.set(bool(state) and state.get('dlss') == dlss.TAG)
        if self.game == 'bananza' and not self.in_place_touched:
            self.in_place_var.set(bool(state) and bool(state.get('sources', {}).get('romfs_in_place')))
        if state is not None:
            self.update_hint.configure(text=self.cat('ui.update_mode'))
            recorded = ini.canonical_language(state.get('lang'))
            if recorded in ini.LANGUAGES and not self.lang_touched:
                self.vars['lang'].set(self.lang_label(recorded))
        else:
            self.update_hint.configure(text='')
        if self.form_amiibo is not None:
            if state is not None:
                self.refresh_amiibo()
                self.form_amiibo.pack(fill='x', pady=(8, 0), after=self.update_hint)
            else:
                self.form_amiibo.pack_forget()
        self.fit()

    def pick_file(self, name, pick_id):
        if self.game == 'bananza' and name == 'nup':
            types = [(self.cat('dkb.ui.type_nup'), '*.nup *.nx'), (self.cat('ui.type_all'), '*.*')]
            path = filedialog.askopenfilename(title=self.cat(pick_id), filetypes=types)
            if path:
                self.vars[name].set(os.path.normpath(path))
            return
        if self.game == 'bananza':
            path = filedialog.askdirectory(title=self.cat(pick_id), mustexist=True)
            if path:
                self.vars[name].set(os.path.normpath(path))
            return
        types = [(self.cat('ui.type_game'), '*.nsp *.xci'), (self.cat('ui.type_all'), '*.*')]
        if name == 'update':
            types = [(self.cat('ui.type_nsp'), '*.nsp'), (self.cat('ui.type_all'), '*.*')]
        if name == 'keys':
            types = [(self.cat('ui.type_keys'), '*.keys'), (self.cat('ui.type_all'), '*.*')]
        path = filedialog.askopenfilename(title=self.cat(pick_id), filetypes=types)
        if path:
            self.vars[name].set(os.path.normpath(path))

    def pick_dest(self):
        path = filedialog.askdirectory(title=self.cat('ui.pick_dest'), mustexist=False)
        if path:
            self.vars['dest'].set(os.path.normpath(path))

    # ---- run ------------------------------------------------------------------------------------------------------

    def start(self):
        form = self.form()
        problems = form.problems()
        if problems:
            self.form_error.configure(text=' '.join(self.cat(p) for p in problems))
            self.fit()
            return
        self.form_error.configure(text='')
        if self.launcher:   # the home screen and the next update use this folder
            self.settings.dest = os.path.normpath(form.dest.strip())
            self.settings.save(self.launcher.base)
        command = form.command(self.layout.python, root=self.layout.root if self.layout.dev else None,
                               temp=self.run_args.get('temp'), verify=self.run_args.get('verify', False),
                               tools=self.run_args.get('tools'))
        self.begin(command, self.layout.root)

    def begin(self, command, cwd):
        self.model = Model(DKB_STEP_ORDER if self.game == 'bananza' else None)
        self.model.dest = self.vars['dest'].get().strip()
        self.clear_log()
        self.build_step_rows(self.model.steps)
        self.cancel_button.configure(state='normal')
        self.bound = [b for b in self.bound if b[0] is not self.cancel_button]
        self.bind_text(self.cancel_button, 'text', 'ui.cancel')
        self.show('run')
        self.refresh_run()
        self.process = Process(command, cwd=cwd, helper_python=self.layout.python)
        self.root.after(POLL_MS, self.poll)

    def build_step_rows(self, steps):
        for w in self.steps_box.winfo_children():
            w.destroy()
        self.step_rows = {}
        for s in steps:
            row = ttk.Frame(self.steps_box)
            row.pack(fill='x', pady=1)
            glyph = ttk.Label(row, width=3)
            glyph.pack(side='left')
            name = ttk.Label(row)
            name.pack(side='left')
            self.step_rows[s] = (glyph, name)

    def poll(self):
        if self.process is None:
            return
        try:
            while True:
                line = self.process.lines.get_nowait()
                if line is not None:
                    self.feed(line)
        except Exception:
            pass
        code = self.process.poll()
        self.refresh_run()
        if code is None:
            self.root.after(POLL_MS, self.poll)
            return
        self.model.finish(code)
        self.process = None
        self.refresh_run()
        self.refresh_finish()
        self.show('finish')
        if self.closing:
            self.root.destroy()

    def feed(self, line):
        """One stdout line of the CLI: an event updates the model and the log, anything else goes to the log as is."""
        event = parse_line(line)
        if event is None:
            if line.strip():
                self.log_entries.append(line.rstrip())
                self.append_log(line.rstrip())
            return
        before = list(self.model.steps)
        self.model.apply(event)
        if self.model.steps != before:
            self.build_step_rows(self.model.steps)
        self.log_entries.append(event)
        self.append_log(describe_event(self.cat, event))

    def refresh_run(self):
        m = self.model
        for s, (glyph, name) in self.step_rows.items():
            state = m.states.get(s, PENDING)
            glyph.configure(text=GLYPH[state], foreground=COLOR[state])
            name.configure(text=self.cat('step.' + s) + (f'  ({self.cat("ui.state_" + state)})'
                                                          if state in (SKIPPED, FAILED) else ''),
                           font=(self.ui_font, 10, 'bold') if state == RUNNING else (self.ui_font, 10))
        mode = m.mode or 'install'
        self.run_header.configure(text=self.cat('ui.running_' + mode if mode in ('install', 'resume', 'update')
                                                else 'ui.running_install'))
        if m.current and m.phase:
            text = self.cat(f'phase.{m.current}.{m.phase}')
            if m.total:
                text += '  ' + amount(self.cat, m.done, m.total, m.unit)
        elif m.current:
            text = self.cat('step.' + m.current)
        else:
            text = ''
        self.phase_label.configure(text=text)
        frac = m.fraction()
        if frac is None:
            if str(self.bar.cget('mode')) != 'indeterminate':
                self.bar.configure(mode='indeterminate')
                self.bar.start(15)
        else:
            if str(self.bar.cget('mode')) == 'indeterminate':
                self.bar.stop()
                self.bar.configure(mode='determinate')
            self.bar.configure(value=frac * 1000)
        self.space_label.configure(text=m.need_text(self.cat))
        self.warn_label.configure(text='\n'.join(self.cat(i, **p) for i, p in m.warnings))
        self.fit()   # new step rows and warnings make the screen taller

    def toggle_log(self):
        self.log_open = not self.log_open
        if self.log_open:
            self.log_frame.pack(fill='both', expand=True, pady=(8, 0))
        else:
            self.log_frame.pack_forget()
        self.bound = [b for b in self.bound if b[0] is not self.log_toggle]
        self.bind_text(self.log_toggle, 'text', 'ui.hide_log' if self.log_open else 'ui.show_log')
        self.fit()

    def clear_log(self):
        self.log_entries = []
        self.render_log()

    def render_log(self):
        """The whole log from the kept events in the current language (raw CLI lines stay as they are)."""
        self.log.configure(state='normal')
        self.log.delete('1.0', 'end')
        for entry in self.log_entries:
            text = entry if isinstance(entry, str) else describe_event(self.cat, entry)
            if text:
                self.log.insert('end', text + '\n')
        self.log.see('end')
        self.log.configure(state='disabled')

    def append_log(self, text):
        if text:
            self.log.configure(state='normal')
            self.log.insert('end', text + '\n')
            self.log.see('end')
            self.log.configure(state='disabled')

    def cancel(self):
        if self.process:
            self.cancel_button.configure(state='disabled')
            self.bound = [b for b in self.bound if b[0] is not self.cancel_button]
            self.bind_text(self.cancel_button, 'text', 'ui.cancelling')
            self.process.cancel()

    def on_close(self):
        if self.precompile_running:
            if not self.precompile_closing and messagebox.askyesno(self.cat('ui.title'),
                                                                   self.cat('ui.confirm_cancel_precompile')):
                self.precompile_closing = True
                self.precompile_stop.set()
            return
        if self.launcher and self.update_running:
            # The download stops at its next block and its .part file is deleted (update.fetch_to); an unpack is not
            # cut half way, and a finished one starts nothing. The window closes when the worker is done.
            if not self.update_closing and messagebox.askyesno(self.cat('ui.title'), self.cat('ui.confirm_cancel_update')):
                self.update_closing = True
                self.update_stop.set()
            return
        if self.process:
            if not messagebox.askyesno(self.cat('ui.title'), self.cat('ui.confirm_cancel')):
                return
            self.closing = True
            self.cancel()
            return
        self.root.destroy()

    # ---- finish ---------------------------------------------------------------------------------------------------

    def refresh_finish(self):
        m = self.model
        result = m.result or 'ok'
        self.finish_title.configure(text=self.cat('ui.result_' + result))
        if result == 'ok':
            text = self.cat('ui.done_text', dest=m.dest)
        elif result == 'cancelled':
            text = self.cat('ui.cancelled_text')
        else:
            err = m.error or ('ui.process_died', {'code': m.exit_code}, None)
            text = self.cat(err[0], **err[1])
            if m.log_path:
                text += '\n\n' + self.cat('ui.log_file', path=m.log_path)
        self.finish_text.configure(text=text)
        for w in self.finish_steps.winfo_children():
            w.destroy()
        for s in m.steps:
            state = m.states.get(s, PENDING)
            ttk.Label(self.finish_steps, text=f'{GLYPH[state]}  {self.cat("step." + s)}',
                      foreground=COLOR[state]).pack(anchor='w')
        if result == 'ok':
            self.open_button.state(['!disabled'])
        else:
            self.open_button.state(['disabled' if not Path(m.dest or '.').is_dir() else '!disabled'])
        self.refresh_build()
        if result == 'ok' and self.game == 'bananza' and install_state(m.dest or '') == 'ready':
            self.play_after.pack(side='right', padx=(0, 8))
            self.uninstall_button.pack(side='left', padx=(8, 0))
        else:
            self.play_after.pack_forget()
            self.uninstall_button.pack_forget()

    def build_folder(self):
        """The kept work folder of the Bananza install just made, or None when there is none."""
        if self.game != 'bananza' or not self.model.dest:
            return None
        try:
            folder = bananza.work_folder(Path(self.model.dest).resolve(), self.run_args.get('temp'), self.layout.temp)
        except InstallError:
            return None
        return folder if folder.is_dir() else None

    def refresh_build(self):
        folder = self.build_folder()
        if folder is None:
            self.finish_build.pack_forget()
            return
        self.finish_build_text.configure(text=self.cat('ui.build_kept', path=folder,
                                                       size=f'{bananza.folder_size(folder) / 2**30:.1f} GB'))
        self.finish_build.pack(fill='x', pady=(8, 0))

    def delete_build(self):
        try:
            bananza.clean(Path(self.model.dest), self.run_args.get('temp'), self.layout.temp)
        except InstallError as e:
            self.finish_build_text.configure(text=self.cat(e.id, **e.params))
            return
        self.refresh_build()

    def uninstall(self):
        dest = Path(self.model.dest)
        title = self.cat('ui.title')
        if not messagebox.askyesno(title, self.cat('ui.uninstall_confirm', path=dest)):
            return
        saves = messagebox.askyesno(title, self.cat('ui.uninstall_saves'), default='no')
        try:
            bananza.uninstall(dest, self.run_args.get('temp'), self.layout.temp, keep_saves=not saves)
        except InstallError as e:
            messagebox.showerror(title, self.cat(e.id, **e.params))
            return
        self.refresh_build()
        self.play_after.pack_forget()
        self.uninstall_button.pack_forget()

    def back(self):
        """From the finish screen: the launcher's home when the folder is built, else the form."""
        if self.launcher and install_state(self.settings.dest):
            self.show_home()
        else:
            self.show('form')

    def open_folder(self):
        dest = self.model.dest
        if dest and Path(dest).is_dir():
            if os.name == 'nt':
                os.startfile(dest)
            else:
                subprocess.Popen(['xdg-open', dest], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                                 start_new_session=True)


# ---- self test ---------------------------------------------------------------------------------------------------

SAMPLE_START = {'type': 'start', 'release': 'selftest', 'mode': 'install', 'dest': 'Z:/selftest/TotK', 'log': 'D:\\log.txt',
                'steps': ['check', 'extract', 'lift', 'compile', 'prepare', 'cleanup']}
SAMPLE_RUN = [
    SAMPLE_START,
    {'type': 'plan', 'run': ['check', 'extract'], 'skip': {}},
    {'type': 'step', 'step': 'check', 'state': 'started'},
    {'type': 'progress', 'step': 'check', 'phase': 'inputs'},
    {'type': 'space', 'dest': 'Z:/selftest/TotK', 'need': 36_400_000_000, 'free': 210_000_000_000, 'temp': 'C:\\t',
     'temp_need': 0, 'temp_free': 0, 'same_disk': True},
    {'type': 'warning', 'id': 'warn.lang_needs_fonts', 'params': {'lang': 'ko'}},
    {'type': 'step', 'step': 'check', 'state': 'done', 'seconds': 12.0},
    {'type': 'step', 'step': 'extract', 'state': 'started'},
    {'type': 'progress', 'step': 'extract', 'phase': 'copy', 'done': 6_000_000_000, 'total': 16_000_000_000,
     'unit': 'bytes'},
]


SAMPLE_OK = [SAMPLE_START, {'type': 'plan', 'run': ['check', 'extract'], 'skip': {'lift': 'current', 'compile': 'current',
                                                                       'prepare': 'current', 'cleanup': 'not_needed'}},
             {'type': 'step', 'step': 'check', 'state': 'started'},
             {'type': 'step', 'step': 'check', 'state': 'done', 'seconds': 1.0},
             {'type': 'step', 'step': 'extract', 'state': 'started'},
             {'type': 'step', 'step': 'extract', 'state': 'done', 'seconds': 1.0},
             {'type': 'step', 'step': 'lift', 'state': 'skipped', 'reason': 'current'},
             {'type': 'step', 'step': 'compile', 'state': 'skipped', 'reason': 'current'},
             {'type': 'step', 'step': 'prepare', 'state': 'skipped', 'reason': 'current'},
             {'type': 'step', 'step': 'cleanup', 'state': 'done', 'seconds': 0.1},
             {'type': 'end', 'result': 'ok', 'seconds': 3}]


def grab(root, path):
    if ImageGrab is None:
        raise SystemExit('--shots needs Pillow')
    root.attributes('-topmost', True)
    root.update()
    x, y, w, h = root.winfo_rootx(), root.winfo_rooty(), root.winfo_width(), root.winfo_height()
    ImageGrab.grab(bbox=(x, y, x + w, y + h), all_screens=True).save(path)


def selftest(layout, shots=None, game='totk'):
    """Every screen in every language; the window closes itself. Returns 0 when nothing raised."""
    fix_tcl_paths()
    fix_dpi_awareness()
    root = tk.Tk()
    root.geometry('+60+40')
    work = tempfile.TemporaryDirectory()   # the launcher's base and a finished-looking install folder, no network
    launcher = None
    if game == 'bananza':
        launcher = Launcher(base=Path(work.name) / 'DKB-Recomp')
        built = Path(work.name) / 'DKB-Bananza'
        built.mkdir()
        (built / 'dkb-install.json').write_text(json.dumps({'format': 1, 'steps': {'dkb_verify': {}}}), encoding='utf-8')
        (built / play_command(built)[0]).write_bytes(b'')
        Settings(dest=str(built)).save(launcher.base)
        before = update.version_python(launcher.base / 'versions' / 'v0.0.1')   # a previous version: Go back shows
        before.parent.mkdir(parents=True)
        before.write_bytes(b'')
        update.write_pointer(launcher.base, 'previous', 'v0.0.1')
    app = App(root, 'en', layout, game=game, launcher=launcher)
    steps, mixed = [], []
    destination = 'Z:/selftest/Bananza' if game == 'bananza' else 'Z:/selftest/TotK'
    tools = update.Asset('0' * 64, 131 << 20, 'DKB-Setup-9.9.9+abc1234-windows.zip', 'DKB-Setup')
    offer = update.Offer('v9.9.9', '9.9.9+abc1234', 1, '## What is new\n\n- Faster loading\n- A crash fixed (#1)',
                         'https://example.invalid/v9.9.9',
                         update.Manifest('9.9.9+abc1234', 'v9.9.9', tools, update.Asset('0' * 64, 1, 'DKB-Launcher.exe')))

    def launcher_screens(tag):
        app.show_offer(offer, None)
        app.show_home()
        shot(f'home-offer-{tag}')
        app.update_step = ('ui.update_downloading', {'done': 40, 'total': 131})
        app.update_target = ('ui.updating', offer.tag)
        app.update_failure = None
        app.update_progress(app.update_step, 0.3)
        app.show('update')
        shot(f'update-{tag}')
        app.update_failed(('update.checksum', {'name': tools.name}))
        shot(f'update-failed-{tag}')
        app.show_offer(None, 'no answer')
        app.show_home()
        shot(f'home-offline-{tag}')
        app.precompile_running = True
        app.precompile_failure = None
        app.precompile_step = ('ui.precompile_warm_count', {'done': 1024, 'total': 4096})
        app.precompile_fraction = 0.25
        app.refresh_precompile()
        app.show('precompile')
        shot(f'precompile-{tag}')
        app.precompile_failed('exit code 1')
        shot(f'precompile-failed-{tag}')
        app.precompile_running = False
        app.precompile_failure = None
        keys = ('ui.home_ready', 'ui.play', 'ui.check_updates', 'ui.updating', 'ui.precompile_title',
                'ui.precompile_note', 'ui.precompile_checking', 'ui.precompile_census', 'ui.precompile_translate',
                'ui.precompile_pack', 'ui.precompile_warm', 'ui.precompile_warm_count', 'ui.precompile_failed',
                'ui.play_anyway', 'ui.skip_and_play', 'ui.confirm_cancel_precompile')
        return all(app.cat(i) != i for i in keys)

    def shot(name):
        root.update()
        if shots:
            Path(shots).mkdir(parents=True, exist_ok=True)
            grab(root, Path(shots) / f'{name}.png')

    def run_all():
        for lang in UI_LANGUAGES:
            app.ui_var.set(lang)
            app.switch_language()
            tag = lang
            app.vars['dest'].set(destination)
            app.vars['base'].set('Z:/selftest/totk.xci')
            app.vars['update'].set('Z:/selftest/totk-1.2.1.nsp')
            app.vars['keys'].set('Z:/selftest/prod.keys')
            if game == 'bananza':
                app.vars['exefs'].set('Z:/selftest/Bananza/exefs')
                app.vars['romfs'].set('Z:/selftest/Bananza/romfs')
            app.show('form')
            app.start()   # missing files: shows the problems
            steps.append(app.form_error.cget('text'))
            shot(f'form-{tag}')
            app.model = Model()
            app.clear_log()
            app.build_step_rows(app.model.steps)
            app.show('run')
            for e in samples(SAMPLE_RUN, game):
                app.feed(json.dumps(e))
            app.log_open = False
            app.toggle_log()
            app.refresh_run()
            shot(f'run-{tag}')
            if lang == 'en':   # the log follows the window language: no English line may remain after the switch
                app.ui_var.set('ru')
                app.switch_language()
                text = app.log.get('1.0', 'end')
                mixed.append('Checking' in text or (game == 'totk' and 'Extracting' not in text and 'Распаковка' not in text))
                app.ui_var.set('en')
                app.switch_language()
            app.model = Model()
            for e in samples(SAMPLE_OK, game):
                app.model.apply(e)
            app.model.dest = destination
            app.model.finish(0)
            app.refresh_finish()
            app.show('finish')
            shot(f'finish-ok-{tag}')
            app.model = Model()
            for e in samples(SAMPLE_RUN[:3], game) + [{'type': 'error', 'id': 'check.disk_space', 'step': 'check',
                                        'params': {'path': destination, 'need_gb': 36.4, 'free_gb': 20.1}},
                                       {'type': 'end', 'result': 'failed', 'seconds': 3}]:
                app.model.apply(e)
            app.model.finish(1)
            app.refresh_finish()
            shot(f'finish-failed-{tag}')
            app.model = Model()
            for e in samples(SAMPLE_RUN[:3], game):
                app.model.apply(e)
            app.model.finish(3)
            app.refresh_finish()
            shot(f'finish-cancelled-{tag}')
            app.toggle_log()
            if launcher:
                screens.append(launcher_screens(tag))
        root.destroy()

    screens = []
    root.after(200, run_all)
    root.after(60000, root.destroy)   # never leaves a window open
    root.mainloop()
    work.cleanup()
    ok = len(steps) == len(UI_LANGUAGES) and all(steps) and mixed == [False]
    return 0 if ok and all(screens) and (launcher is None or len(screens) == len(UI_LANGUAGES)) else 1


def samples(events, game):
    """Keep the same screen fixtures for both profiles, with each profile's actual step names."""
    if game != 'bananza':
        return events
    mapping = {'extract': 'dkb_data', 'prepare': 'dkb_verify'}
    result = []
    for original in events:
        e = dict(original)
        if 'dest' in e:
            e['dest'] = 'Z:/selftest/Bananza'
        if e.get('step') in mapping:
            e['step'] = mapping[e['step']]
        if e['type'] == 'start':
            e['steps'] = list(DKB_STEP_ORDER)
            e['dest'] = 'Z:/selftest/Bananza'
        if e['type'] == 'plan':
            e['run'] = [mapping.get(s, s) for s in e.get('run', [])]
            e['skip'] = {mapping.get(s, s): reason for s, reason in e.get('skip', {}).items()}
        if e['type'] == 'progress':
            e['phase'] = 'dkb_run' if e['step'] != 'check' else 'dkb_base'
        result.append(e)
    if any(e['type'] == 'end' and e.get('result') == 'ok' for e in result):
        # A fresh successful Bananza install completed every real stage; show no pending or fictitious skipped work.
        start = next(e for e in result if e['type'] == 'start')
        result = [start, {'type': 'plan', 'run': list(DKB_STEP_ORDER), 'skip': {}}]
        for step in DKB_STEP_ORDER:
            result += [{'type': 'step', 'step': step, 'state': 'started'},
                       {'type': 'step', 'step': step, 'state': 'done', 'seconds': 1.0}]
        result.append({'type': 'end', 'result': 'ok', 'seconds': float(len(DKB_STEP_ORDER))})
    return result


def start_launcher(args, layout):
    """What a window the launcher started does first. A window of one of the base folder's versions confirms its start
    (installer/update.py confirm_start: it becomes current.txt; the old version stays as previous.txt) and puts its own
    DKB-Setup.exe, checked with the zip it came in, in place of the launcher exe when they differ."""
    launcher = Launcher(base=args.base.resolve(), exe=args.launcher, after_update=args.after_update,
                        allow_downgrade=args.allow_downgrade)
    tag = update.own_tag(launcher.base, layout.root)
    if tag is None:
        return launcher
    if update.confirm_start(launcher.base, tag):
        launcher.note(f'{tag} ({layout.release}) started and is now current')
    if launcher.exe and os.name == 'nt':
        update.clean_old_launcher(launcher.exe)
        try:
            if update.replace_launcher(launcher.exe, layout.root / update.LAUNCHER_EXE):
                launcher.note(f'{launcher.exe} replaced by the launcher of {tag}')
        except (OSError, update.UpdateError) as e:   # the old launcher keeps working: it starts current.txt's version
            launcher.note(f'{launcher.exe} not replaced: {e}')
    return launcher


def play_now(dest, layout=None):
    """--play: warm missing shader pipelines if needed (--warm-shaders-if-needed, progress on stdout), then start the game in dest and wait for it (#402);
    1 when dest holds no finished install, as the Play button would stay disabled."""
    if sys.stderr is None:   # pythonw.exe started with no handles
        sys.stdout = sys.stderr = open(os.devnull, 'w')
    if install_state(dest) != 'ready':
        print(f'--play: no finished install in {dest or "(no folder given or remembered)"}', file=sys.stderr)
        return 1
    dest_path = Path(dest)
    if (dest_path / GAME_EXE).is_file() and (dest_path / 'shader-prepared.bin').is_file() and not libraries_failed(dest_path):
        try:
            _run_precompile_proc(warm_command(dest_path), dest_path, on_line=lambda line: print(line, end='', flush=True))
        except OSError as e:
            print(f'--play: {e}', file=sys.stderr)
    try:
        return subprocess.call(play_command(dest), cwd=dest)
    except OSError as e:
        print(f'--play: {play_command(dest)[0]}: {e}', file=sys.stderr)
        return 1


# Options only the command-line setup takes. DKB-Setup.exe and the Linux AppImage start this window with their own
# arguments, so a frontend that passes one of these gets `python -m installer` (cli.py) with the whole command line
# instead of a window that rejects it (#402).
CLI_OPTIONS = ('--dest', '--source', '--prepare', '--nup', '--exefs', '--romfs', '--keys', '--update', '--verify', '--clean', '--uninstall')


def prefill_form(app, args):
    if args.dest:
        app.vars['dest'].set(os.path.normpath(args.dest))
    if args.source:
        from . import cli
        found = cli.source_inputs(args.source)   # None: neither a file nor a folder with exefs/ and romfs/; leave blank
        for name, path in zip(('nup', 'exefs', 'romfs'), found or ()):
            if path:
                app.vars[name].set(os.path.normpath(path))


def main(argv=None):
    argv = sys.argv[1:] if argv is None else argv
    given = {a.split('=', 1)[0] for a in argv if a.startswith('--')}
    # Only --source and/or --dest (and --lang): open the window with those fields filled in instead of running the setup.
    prefill = bool(given & {'--source', '--dest'}) and given <= {'--source', '--dest', '--lang'}
    if not prefill and any(a.split('=', 1)[0] in CLI_OPTIONS for a in argv):
        if sys.stdout is None:   # pythonw.exe started with no handles: nobody reads the lines, the exit code still counts
            sys.stdout = sys.stderr = open(os.devnull, 'w')
        from . import cli
        return cli.main(argv)
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--selftest', action='store_true')
    ap.add_argument('--shots', type=Path)
    ap.add_argument('--lang', choices=UI_LANGUAGES)
    ap.add_argument('--root', type=Path)
    ap.add_argument('--game', choices=('bananza', 'totk'), default='bananza')
    for name in ('package', 'toolchain', 'deps', 'ninja', 'dll-dir', 'translator', 'dotnet'):
        ap.add_argument('--' + name, type=Path)
    ap.add_argument('--jobs', type=int, default=6)
    ap.add_argument('--source', type=Path, help='prefill the game field: a .nup file, or a folder with exefs/ and romfs/')
    ap.add_argument('--dest', type=Path, help='prefill the install folder')
    ap.add_argument('--no-update-check', action='store_true', help='do not look for a newer release (or DKB_NO_UPDATE_CHECK=1)')
    ap.add_argument('--base', type=Path, help='the launcher folder (DKB-Recomp; launcher.c passes it)')
    ap.add_argument('--launcher', type=Path, help='the launcher exe that started this window')
    ap.add_argument('--after-update', action='store_true', help='started by the version before: rebuild the install folder')
    ap.add_argument('--allow-downgrade', action='store_true', help='also offer a latest release older than this one')
    ap.add_argument('--play', nargs='?', const='', metavar='DIR',
                    help='start the finished game in DIR (default: the install folder the launcher remembers) with no '
                         'window, wait for it and return its exit code')
    args = ap.parse_args(argv)
    layout = layouts.find(args.root)
    if args.selftest:
        return selftest(layout, args.shots, args.game)
    if args.play is not None:
        return play_now(args.play or (Settings.load(args.base).dest if args.base else ''), layout=layout)
    fix_tcl_paths()
    fix_dpi_awareness()
    launcher = start_launcher(args, layout) if args.base else None
    root = tk.Tk()
    tools = {name: getattr(args, name) for name in ('toolchain', 'deps', 'ninja', 'dll_dir', 'translator', 'dotnet', 'jobs')}
    check = not update.disabled(args.no_update_check) and (launcher is None or Settings.load(launcher.base).check_updates)
    app = App(root, args.lang or windows_ui_language(), layout, run_args={'tools': tools}, game=args.game,
              check_updates=check, launcher=launcher)
    if args.package:
        app.vars['package'].set(str(args.package))
    prefill_form(app, args)
    root.mainloop()
    return 0


if __name__ == '__main__':
    sys.exit(main())
