"""The installer window: event model, form, catalogs, cancel, a self-closing Tk run and one end-to-end run.

    .venv-py314\\Scripts\\python tests\\installer_gui_test.py

The end-to-end test runs the real CLI in --verify mode on a finished install (every step skips) and folds its events
through the same code the window uses (the fixture's step keys are recomputed for the checkout under test, because the keys fingerprint recomp/, tools/ and runtime/). The install is build/inst-gui/e2e-install: hard links to build/install-test, so
nothing there is written, except the state file and dkb.ini, which a run rewrites and which are copied. The run hashes
the 17 GB RomFS; it is skipped when the hash differs from the expected one in all three tries. On the developer machine
(RAM with a bad bit) a buffered read of a file sometimes returns flipped bits from the page cache while the file on
disk is intact (an unbuffered read of install-test's romfs.bin gives the expected hash), so such a skip is the machine,
not the installer. It is also skipped when build/install-test is absent.
"""
import io
import json
import os
import queue
import re
import shutil
import subprocess
import sys
import tempfile
import time
import unittest
from pathlib import Path
from types import SimpleNamespace

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))
from installer import layout as layouts  # noqa: E402
from installer import state as statefile  # noqa: E402
from installer.events import Reporter  # noqa: E402
from installer.runner import Context  # noqa: E402
from installer.steps import all_steps  # noqa: E402
from installer.guilogic import (DONE, FAILED, PENDING, RUNNING, SKIPPED, UI_LANGUAGES,  # noqa: E402
                                Catalog, Form, Model, Process, amount, describe_event, parse_line,
                                windows_ui_language)

INSTALLED = ROOT / 'build' / 'install-test'
FIXTURE = ROOT / 'build' / 'inst-gui' / 'e2e-install'
COPIED = ('dkb.ini', statefile.NAME)   # written by a run, so never linked
START = {'type': 'start', 'release': 'r1', 'mode': 'install', 'dest': 'D:\\T', 'log': 'D:\\T.log',
         'steps': ['check', 'extract', 'lift', 'compile', 'prepare', 'cleanup']}


def fold(*events):
    m = Model()
    for e in events:
        m.apply(e)
    return m


class ParseTest(unittest.TestCase):
    def test_event_line(self):
        self.assertEqual(parse_line('{"type": "step", "step": "check", "state": "started"}\n')['step'], 'check')

    def test_noise_is_not_an_event(self):
        for line in ('', 'Traceback (most recent call last):', '{broken', '[1, 2]', '{"no_type": 1}', '{"type": 3}'):
            self.assertIsNone(parse_line(line), line)

    def test_unicode_survives(self):
        e = parse_line(json.dumps({'type': 'warning', 'id': 'x', 'params': {'p': 'Привет'}}, ensure_ascii=False))
        self.assertEqual(e['params']['p'], 'Привет')


class ForwardTest(unittest.TestCase):
    def test_cli_options_go_to_the_command_line_setup(self):
        from unittest import mock
        from installer import gui
        with mock.patch('installer.cli.main', return_value=7) as cli_main:
            self.assertEqual(gui.main(['--source', 'x.nup', '--dest=out', '--prepare']), 7)
            cli_main.assert_called_once_with(['--source', 'x.nup', '--dest=out', '--prepare'])

    def test_source_and_dest_alone_prefill_the_window(self):
        from types import SimpleNamespace
        from unittest import mock
        from installer import gui
        with tempfile.TemporaryDirectory() as d:
            nup = Path(d) / 'game.nup'
            nup.write_bytes(b'x')
            vars = {n: mock.Mock() for n in ('nup', 'exefs', 'romfs', 'package', 'dest')}
            app = SimpleNamespace(vars=vars)
            with mock.patch('installer.cli.main') as cli_main, mock.patch.object(gui.tk, 'Tk'),                     mock.patch.object(gui, 'App', return_value=app) as make, mock.patch.object(gui, 'fix_tcl_paths'),                     mock.patch.object(gui, 'fix_dpi_awareness'), mock.patch.object(gui, 'windows_ui_language', return_value='en'):
                self.assertEqual(gui.main(['--source', str(nup), '--dest=out', '--lang', 'en']), 0)
                make.assert_called_once()
                cli_main.assert_not_called()
                vars['nup'].set.assert_called_once_with(os.path.normpath(nup))
                vars['dest'].set.assert_called_once_with('out')
                vars['exefs'].set.assert_not_called()
                # one option more and it is the command line again
                cli_main.return_value = 3
                self.assertEqual(gui.main(['--source', str(nup), '--prepare']), 3)

    def test_play_needs_a_finished_install(self):
        from unittest import mock
        from installer import gui
        with tempfile.TemporaryDirectory() as d, mock.patch('subprocess.call', return_value=5) as call:
            self.assertEqual(gui.main(['--play', d]), 1)   # empty folder: nothing to start
            call.assert_not_called()
            with mock.patch.object(gui, 'install_state', return_value='ready'):
                self.assertEqual(gui.main(['--play', d]), 5)
            self.assertEqual(call.call_args.kwargs['cwd'], d)


class ModelTest(unittest.TestCase):
    def test_steps_follow_the_events(self):
        m = fold(START, {'type': 'plan', 'run': ['check', 'extract'], 'skip': {'lift': 'current', 'compile': 'current'}},
                 {'type': 'step', 'step': 'check', 'state': 'started'})
        self.assertEqual([m.states[s] for s in m.steps],
                         [RUNNING, PENDING, SKIPPED, SKIPPED, PENDING, PENDING])
        self.assertEqual(m.current, 'check')
        m.apply({'type': 'step', 'step': 'check', 'state': 'done', 'seconds': 1.0})
        self.assertEqual(m.states['check'], DONE)
        self.assertIsNone(m.current)
        m.apply({'type': 'step', 'step': 'extract', 'state': 'skipped', 'reason': 'current'})
        self.assertEqual(m.states['extract'], SKIPPED)

    def test_progress_fraction(self):
        m = fold(START, {'type': 'step', 'step': 'extract', 'state': 'started'},
                 {'type': 'progress', 'step': 'extract', 'phase': 'copy', 'done': 5, 'total': 20, 'unit': 'bytes'})
        self.assertEqual((m.current, m.phase, m.fraction()), ('extract', 'copy', 0.25))
        m.apply({'type': 'progress', 'step': 'extract', 'phase': 'merge'})
        self.assertIsNone(m.fraction())   # a phase without size is indeterminate
        m.apply({'type': 'progress', 'step': 'extract', 'phase': 'copy', 'done': 30, 'total': 20})
        self.assertEqual(m.fraction(), 1.0)

    def test_a_new_step_resets_progress(self):
        m = fold(START, {'type': 'progress', 'step': 'check', 'phase': 'inputs', 'done': 1, 'total': 2},
                 {'type': 'step', 'step': 'extract', 'state': 'started'})
        self.assertIsNone(m.phase)
        self.assertIsNone(m.fraction())

    def test_error_marks_the_step(self):
        m = fold(START, {'type': 'step', 'step': 'check', 'state': 'started'},
                 {'type': 'error', 'id': 'check.disk_space', 'step': 'check', 'params': {'path': 'D:\\T'}},
                 {'type': 'end', 'result': 'failed', 'seconds': 2})
        self.assertEqual(m.states['check'], FAILED)
        self.assertEqual(m.error[0], 'check.disk_space')
        self.assertEqual(m.result, 'failed')

    def test_space_text(self):
        m = fold({'type': 'space', 'dest': 'D:\\T', 'need': 36_400_000_000, 'free': 100_000_000_000, 'temp': 'C:\\t',
                  'temp_need': 0, 'temp_free': 5, 'same_disk': True})
        text = m.need_text(Catalog('en'))
        self.assertIn('36.4', text)
        self.assertIn('100.0', text)
        self.assertEqual(Model().need_text(Catalog('en')), '')
        m = fold({'type': 'space', 'dest': 'D:\\T', 'need': 20_000_000_000, 'free': 1, 'temp': 'C:\\t',
                  'temp_need': 2_000_000_000, 'temp_free': 3, 'same_disk': False})
        self.assertIn('C:\\t', m.need_text(Catalog('en')))

    def test_warnings_collect(self):
        m = fold(START, {'type': 'warning', 'id': 'warn.lang_needs_fonts', 'params': {'lang': 'ko'}})
        self.assertEqual(m.warnings, [('warn.lang_needs_fonts', {'lang': 'ko'})])

    def test_finish_without_end_event(self):
        m = fold(START, {'type': 'step', 'step': 'lift', 'state': 'started'})
        m.finish(-1073741510)
        self.assertEqual(m.result, 'failed')
        self.assertEqual(m.states['lift'], FAILED)
        self.assertEqual(m.error[0], 'ui.process_died')

    def test_finish_after_cancel(self):
        m = fold(START, {'type': 'step', 'step': 'lift', 'state': 'started'}, {'type': 'end', 'result': 'cancelled'})
        m.finish(3)
        self.assertEqual(m.result, 'cancelled')
        self.assertIsNone(m.error)
        self.assertEqual(m.states['lift'], PENDING)   # not finished; the next run resumes here

    def test_exit_code_3_alone_is_cancelled(self):
        m = Model()
        m.finish(3)
        self.assertEqual(m.result, 'cancelled')

    def test_unknown_event_is_ignored(self):
        m = fold(START, {'type': 'future_thing', 'x': 1})
        self.assertEqual(m.states['check'], PENDING)


class RunStatesTest(unittest.TestCase):
    OK = [START, {'type': 'plan', 'run': ['check', 'extract'], 'skip': {'lift': 'current', 'compile': 'current',
                                                                     'prepare': 'current', 'cleanup': 'not_needed'}},
          {'type': 'step', 'step': 'check', 'state': 'started'}, {'type': 'step', 'step': 'check', 'state': 'done'},
          {'type': 'step', 'step': 'extract', 'state': 'started'}, {'type': 'step', 'step': 'extract', 'state': 'done'},
          {'type': 'step', 'step': 'lift', 'state': 'skipped'}, {'type': 'step', 'step': 'compile', 'state': 'skipped'},
          {'type': 'step', 'step': 'prepare', 'state': 'skipped'}, {'type': 'step', 'step': 'cleanup', 'state': 'done'},
          {'type': 'end', 'result': 'ok', 'seconds': 1}]
    FAILED = [START, {'type': 'step', 'step': 'check', 'state': 'started'}, {'type': 'step', 'step': 'check', 'state': 'done'},
              {'type': 'step', 'step': 'extract', 'state': 'started'},
              {'type': 'error', 'id': 'extract.romfs_mismatch', 'step': 'extract', 'params': {}},
              {'type': 'warning', 'id': 'warn.lang_needs_fonts', 'params': {'lang': 'ko'}},
              {'type': 'end', 'result': 'failed', 'seconds': 1}]

    def test_ok_run_has_only_done_and_skipped_steps(self):
        m = fold(*self.OK)
        m.finish(0)
        self.assertEqual(m.result, 'ok')
        self.assertIsNone(m.error)
        self.assertEqual({m.states[s] for s in m.steps}, {DONE, SKIPPED})

    def test_a_new_run_keeps_nothing_of_the_failed_one(self):
        m = fold(*self.FAILED)
        m.finish(1)
        self.assertEqual(m.states['extract'], FAILED)
        for e in self.OK:   # the same model object fed a second run (the window makes a new one, but even then)
            m.apply(e)
        m.finish(0)
        self.assertEqual({m.states[s] for s in m.steps}, {DONE, SKIPPED})
        self.assertIsNone(m.error)
        self.assertEqual(m.warnings, [])
        self.assertEqual(m.result, 'ok')


class FormTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = Path(self.tmp.name)
        for name in ('base.nsp', 'upd.nsp', 'prod.keys'):
            (self.dir / name).write_bytes(b'x')

    def tearDown(self):
        self.tmp.cleanup()

    def form(self, **kw):
        values = dict(base=str(self.dir / 'base.nsp'), update=str(self.dir / 'upd.nsp'), keys=str(self.dir / 'prod.keys'),
                      lang='en-US', dest=str(self.dir / 'out'))
        values.update(kw)
        return Form(**values)

    def test_complete_form(self):
        self.assertEqual(self.form().problems(), [])

    def test_empty_form(self):
        self.assertEqual(Form().problems(), ['ui.need_dest', 'ui.need_base', 'ui.need_update', 'ui.need_keys',
                                             'ui.need_lang'])

    def test_missing_files(self):
        self.assertEqual(self.form(base=str(self.dir / 'no.nsp'), keys=str(self.dir / 'no.keys')).problems(),
                         ['ui.missing_base', 'ui.missing_keys'])

    def test_unknown_language(self):
        self.assertEqual(self.form(lang='xx').problems(), ['ui.need_lang'])

    def test_command(self):
        cmd = self.form().command('py.exe', root=ROOT, temp=self.dir / 't', verify=True)
        self.assertEqual(cmd[:3], ['py.exe', '-m', 'installer'])
        for flag in ('--base', '--update', '--keys', '--lang', '--dest', '--root', '--temp', '--verify'):
            self.assertIn(flag, cmd)
        self.assertEqual(cmd[cmd.index('--lang') + 1], 'en-US')

    def test_installed_folder_needs_only_itself(self):
        out = self.dir / 'out'
        statefile.save(self._mkdir(out), statefile.empty('r0') | {'lang': 'ru'})
        form = Form(dest=str(out))
        self.assertTrue(form.update_mode())
        self.assertEqual(form.problems(), [])
        cmd = form.command('py.exe')
        self.assertEqual(cmd, ['py.exe', '-m', 'installer', '--dest', str(out)])

    def test_installed_folder_still_checks_given_files(self):
        out = self.dir / 'out'
        statefile.save(self._mkdir(out), statefile.empty('r0'))
        self.assertEqual(Form(dest=str(out), base=str(self.dir / 'no.nsp')).problems(), ['ui.missing_base'])

    def test_broken_state_is_a_new_install(self):
        out = self._mkdir(self.dir / 'out')
        (out / statefile.NAME).write_text('{nope', encoding='utf-8')
        self.assertFalse(Form(dest=str(out)).update_mode())

    @staticmethod
    def _mkdir(path):
        path.mkdir(parents=True, exist_ok=True)
        return path


class CatalogTest(unittest.TestCase):
    def test_every_language_covers_english_with_the_same_placeholders(self):
        en = Catalog('en').english
        for lang in UI_LANGUAGES[1:]:
            other = Catalog(lang).texts
            self.assertEqual(sorted(set(en) - set(other)), [], lang)
            self.assertEqual(sorted(set(other) - set(en)), [], lang)
            holes = [k for k in en
                     if sorted(re.findall(r'\{\w+\}', en[k])) != sorted(re.findall(r'\{\w+\}', other[k]))]
            self.assertEqual(holes, [], lang)

    def test_every_id_the_window_uses_exists(self):
        cat = Catalog('en')
        ids = ['ui.' + n for n in ('title', 'start', 'cancel', 'open_folder', 'notice', 'space_one', 'space_two',
                                   'process_died', 'result_ok', 'result_failed', 'result_cancelled')]
        ids += [f'step.{s}' for s in ('check', 'extract', 'lift', 'compile', 'prepare', 'cleanup')]
        ids += ['ui.state_' + n for n in ('started', 'done', 'skipped', 'pending', 'running', 'failed')]
        for lang in UI_LANGUAGES:
            cat.set_language(lang)
            self.assertEqual([i for i in ids if not cat.has(i)], [], lang)

    def test_codes_become_names(self):
        for lang, korean in (('en', 'Korean'), ('ru', 'Корейский'), ('zh', '韩语')):
            cat = Catalog(lang)
            self.assertIn(korean, cat('warn.lang_needs_fonts', lang='ko'))
            self.assertNotIn(' ko ', cat('warn.lang_needs_fonts', lang='ko'))
            self.assertNotIn('extract', cat('state.output_changed', step='extract'))
            for word in ('base', 'update', 'keys'):
                self.assertIn(cat('param.input.' + word), cat('check.keys_wrong', input=word))
            self.assertNotIn('en-US', cat('check.lang_unknown', lang='xx', known='ja, en-US'))
        self.assertEqual(Catalog('en')('check.lang_unknown', lang='xx', known='ja, en-US'),
                         "Unknown language 'xx'. Known: Japanese, English (US).")

    def test_fallback_and_params(self):
        cat = Catalog('ru')
        self.assertIn('36.4', cat('ui.log_space', need='36.4', free='1.0'))
        self.assertEqual(cat('no.such.id'), 'no.such.id')
        self.assertIn('a, b', cat('check.release_incomplete', paths=['a', 'b']))
        cat.set_language('de')   # unknown language falls back to English
        self.assertEqual(cat.lang, 'en')

    def test_describe_and_amount(self):
        cat = Catalog('en')
        self.assertEqual(amount(cat, 6e9, 16e9, 'bytes'), '6.0 / 16.0 GB')
        self.assertEqual(amount(cat, 1, 9, 'shaders'), '1 / 9 shaders')
        self.assertEqual(amount(cat, 1, 9, 'widgets'), '1 / 9 widgets')
        line = describe_event(cat, {'type': 'step', 'step': 'check', 'state': 'done', 'seconds': 1.5})
        self.assertIn('1.5 s', line)
        self.assertEqual(describe_event(cat, {'type': 'start'}), '')

    def test_windows_language_is_a_known_value(self):
        self.assertIn(windows_ui_language(), UI_LANGUAGES)


BREAK_CHILD = 'import time\nprint("ready", flush=True)\ntime.sleep(60)\n'


class CancelTest(unittest.TestCase):
    def test_ctrl_break_ends_the_process_group(self):
        p = Process([sys.executable, '-c', BREAK_CHILD])
        try:
            self.assertEqual(p.lines.get(timeout=20).strip(), 'ready')
            t0 = time.monotonic()
            p.cancel()
            code = None
            while code is None and time.monotonic() - t0 < 20:
                code = p.poll()
                time.sleep(0.05)
            self.assertIsNotNone(code, 'the child ignored Ctrl+Break')
            self.assertNotEqual(code, 0)
        finally:
            p.kill()


class LauncherWindowTest(unittest.TestCase):
    """The launcher's window (installer/gui.py App with a Launcher): closing it during an update, and its worker
    reaching a window that is gone."""

    def setUp(self):
        try:
            import tkinter as tk
            self.root = tk.Tk()
        except Exception as e:   # no tkinter, or no display
            self.skipTest(f'no Tk: {e}')
        self.root.withdraw()
        from installer import gui
        from installer.guilogic import Launcher
        self.gui = gui
        self.work = tempfile.TemporaryDirectory()
        self.addCleanup(self.work.cleanup)
        self.app = gui.App(self.root, 'en', layouts.find(None), game='bananza',
                           launcher=Launcher(base=Path(self.work.name) / 'DKB-Recomp'))

    def tearDown(self):
        try:
            self.root.destroy()
        except Exception:
            pass

    def test_close_during_an_update_asks_then_stops_it(self):
        from unittest.mock import patch
        from installer import update
        app = self.app
        app.begin_update('ui.updating', 'v9.9.9', ('ui.update_downloading', {'done': 0, 'total': 1}))
        with patch.object(self.gui.messagebox, 'askyesno', return_value=False) as asked:
            app.on_close()
        asked.assert_called_once()
        self.assertFalse(app.update_stop.is_set())   # "no": the update goes on, the window stays
        self.assertTrue(self.root.winfo_exists())
        with patch.object(self.gui.messagebox, 'askyesno', return_value=True):
            app.on_close()
        self.assertTrue(app.update_stop.is_set() and app.update_closing)
        self.assertTrue(self.root.winfo_exists())    # it closes once the worker has stopped

        def cancelled():   # what update.download raises once it sees update_stop (and deletes its .part file)
            raise update.UpdateError('update.cancelled')
        app.update_worker_body('update to v9.9.9', cancelled)
        self.assertFalse(app.update_running)
        self.root.update()
        with self.assertRaises(Exception):
            self.root.winfo_exists()   # destroyed
        app.later(app.show_home)       # a worker's last message to a window that is gone is dropped, not raised

    def test_cancel_after_the_last_block_unpacks_and_starts_nothing(self):
        """Cancel pressed while the last block came in: update.download has nothing left to stop and returns the
        checked zip, which is then deleted instead of unpacked; a Cancel that comes during the unpack (before the
        button goes grey) keeps the unpacked folder but starts nothing."""
        from unittest.mock import patch
        from installer import update
        app = self.app
        tools = update.Asset('0' * 64, 1 << 20, 'DKB-Setup-9.9.9-windows.zip', 'DKB-Setup')
        offer = update.Offer('v9.9.9', '9.9.9', 1, manifest=update.Manifest('9.9.9', 'v9.9.9', tools, tools))
        zip_path = Path(self.work.name) / 'checked.zip'
        for cancel_during in ('download', 'stage'):
            zip_path.write_bytes(b'PK')
            stage_calls = []

            def download(*a, **kw):
                if cancel_during == 'download':
                    app.update_stop.set()
                return zip_path

            def stage(*a):
                stage_calls.append(a)
                if cancel_during == 'stage':
                    app.update_stop.set()
                return Path(self.work.name) / 'versions' / 'v9.9.9'
            app.begin_update('ui.updating', 'v9.9.9', ('ui.update_downloading', {'done': 0, 'total': 1}))
            with patch.object(update, 'download', side_effect=download), patch.object(update, 'stage', side_effect=stage), \
                    patch.object(update, 'start_version') as started:
                app.update_worker(offer)
            self.root.update()
            with self.subTest(cancel_during):
                self.assertEqual(len(stage_calls), 0 if cancel_during == 'download' else 1)
                started.assert_not_called()
                self.assertFalse(zip_path.exists())
                self.assertFalse(app.update_running)
                self.assertEqual(app.update_failure[0], 'update.cancelled')
                self.assertTrue(self.root.winfo_exists())   # the window stays: only Close closes it

    def test_a_handed_over_update_closes_the_window(self):
        from unittest.mock import patch
        app = self.app
        app.begin_update('ui.going_back', 'v0.0.1', ('ui.update_starting', {'tag': 'v0.0.1'}))
        with patch.object(self.gui.update, 'start_version', return_value='started') as started:
            app.update_worker_body('go back to v0.0.1', lambda: app.hand_over('v0.0.1'))
        self.assertEqual(started.call_args[0][1], 'v0.0.1')
        self.root.update()
        with self.assertRaises(Exception):
            self.root.winfo_exists()


class SmokeTest(unittest.TestCase):
    def test_every_screen_in_both_languages(self):
        try:
            import tkinter  # noqa: F401
        except ImportError:
            self.skipTest('no tkinter')
        r = subprocess.run([sys.executable, '-m', 'installer.gui', '--selftest'], cwd=ROOT, timeout=120,
                           capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stderr[-2000:])


def make_fixture():
    """build/inst-gui/e2e-install: links to install-test (see the module docstring), with the step keys of the state
    set to the ones this checkout computes, so the install counts as current for it."""
    if not (FIXTURE / statefile.NAME).is_file():
        shutil.rmtree(FIXTURE, ignore_errors=True)
        for folder, _dirs, files in os.walk(INSTALLED):
            target = FIXTURE / Path(folder).relative_to(INSTALLED)
            target.mkdir(parents=True, exist_ok=True)
            for name in files:
                src, dst = Path(folder) / name, target / name
                if name in COPIED and target == FIXTURE:
                    shutil.copy2(src, dst)
                else:
                    os.link(src, dst)
    layout = layouts.find(ROOT)
    state = statefile.load(FIXTURE)
    options = SimpleNamespace(base=None, update=None, keys=None, lang=None)
    ctx = Context(layout, FIXTURE, ROOT / 'build' / 'inst-gui' / 'e2e-temp', Reporter(out=io.StringIO()), options, state=state)
    ctx.steps = all_steps()
    ctx.plan()
    for step_id, record in state['steps'].items():
        record['key'] = ctx.keys[step_id]
    statefile.save(FIXTURE, state)
    return FIXTURE


@unittest.skipUnless((INSTALLED / statefile.NAME).is_file(), 'build/install-test is absent')
class EndToEndTest(unittest.TestCase):
    @staticmethod
    def run_cli(form, temp, tries=1):
        """Runs the CLI in --verify mode through Process, up to `tries` times while the RomFS hash reads wrong."""
        for attempt in range(tries):
            p = Process(form.command(sys.executable, root=ROOT, temp=temp, verify=True), cwd=ROOT)
            model, stray = Model(), []
            t0, code = time.monotonic(), None
            while code is None and time.monotonic() - t0 < 600:
                try:
                    line = p.lines.get(timeout=0.2)
                except queue.Empty:
                    line = ''
                if line:
                    event = parse_line(line)
                    if event:
                        model.apply(event)
                    elif line.strip():
                        stray.append(line)
                code = p.poll()
            p.kill()
            model.finish(code)
            if not any(w[0] == 'state.output_changed' for w in model.warnings):
                break
        return code, model, stray, time.monotonic() - t0

    def test_verify_run_skips_every_step(self):
        form = Form(dest=str(make_fixture()))
        self.assertTrue(form.update_mode())
        code, model, stray, elapsed = self.run_cli(form, ROOT / 'build' / 'inst-gui' / 'e2e-temp', tries=3)
        print(f'\nend-to-end: exit {code}, result {model.result}, {elapsed:.1f} s, states {model.states}')
        if any(w[0] == 'state.output_changed' for w in model.warnings):
            self.skipTest('the RomFS hash read wrong three times (page cache of the faulty RAM, see the module docstring)')
        self.assertEqual(code, 0, stray[-5:] or model.error)
        self.assertEqual(model.result, 'ok')
        self.assertIn(model.mode, ('resume', 'update'))
        self.assertEqual({s: model.states[s] for s in model.steps if s not in ('check', 'cleanup')},
                         {s: SKIPPED for s in model.steps if s not in ('check', 'cleanup')})
        self.assertIsNotNone(model.space)
        self.assertEqual(stray, [])

    def test_cancel_during_the_check(self):
        cmd = Form(dest=str(make_fixture())).command(sys.executable, root=ROOT, temp=ROOT / 'build' / 'inst-gui' / 'e2e-temp',
                                                     verify=True)
        p = Process(cmd, cwd=ROOT)
        model, t0, code, cancelled = Model(), time.monotonic(), None, False
        while code is None and time.monotonic() - t0 < 120:
            try:
                event = parse_line(p.lines.get(timeout=0.2))
            except queue.Empty:
                event = None
            if event:
                model.apply(event)
                if event.get('phase') == 'verify_romfs' and not cancelled:
                    cancelled = True
                    p.cancel()
            code = p.poll()
        p.kill()
        model.finish(code)
        self.assertTrue(cancelled)
        self.assertEqual((code, model.result), (3, 'cancelled'))
        self.assertIsNone(model.error)


if __name__ == '__main__':
    unittest.main(verbosity=2)
