#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Read-only live GTK smoke test. Does not install or restore extensions.
Usage: GSETTINGS_SCHEMA_DIR=... python3 tests/backup-gui-smoke.py BINARY
Requires AT-SPI and a live GNOME desktop with no running fork instance.
"""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time
import gi
gi.require_version('Atspi', '2.0')
from gi.repository import Atspi, GLib


def walk(node, depth=0):
    if depth > 45: return
    yield node
    for i in range(node.get_child_count()):
        child = node.get_child_at_index(i)
        if child: yield from walk(child, depth + 1)


def wait(function, description, timeout=30):
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        try:
            value = function()
            if value: return value
        except GLib.GError:
            pass
        time.sleep(0.2)
    raise AssertionError('Timed out: ' + description)


def window(title):
    return next((n for n in walk(Atspi.get_desktop(0)) if n.get_name() == title and n.get_role_name() in {'frame', 'dialog', 'file chooser'}), None)


def named(root, name, role=None):
    return next((n for n in walk(root) if n.get_name() == name and (not role or n.get_role_name() == role)), None)


def action(node, name):
    for i in range(node.get_n_actions()):
        if node.get_action_name(i) == name:
            assert node.do_action(i)
            return
    raise AssertionError('Missing action ' + name)


binary = str(Path(sys.argv[1]).resolve())
with tempfile.TemporaryDirectory(prefix='exm-backup-gui-', dir='/tmp/opencode') as root:
    environment = dict(os.environ, GSETTINGS_BACKEND='memory', G_DEBUG='fatal-criticals',
                       XDG_CONFIG_HOME=root + '/config', XDG_DATA_HOME=root + '/data',
                       XDG_CACHE_HOME=root + '/cache', LANG='en_US.UTF-8')
    with open(root + '/app.log', 'w+') as log:
        process = subprocess.Popen([binary], env=environment, stdout=log, stderr=log)
        try:
            main = wait(lambda: window('Extension Manager Local'), 'main window')
            wait(lambda: named(main, 'User-Installed Extensions'), 'Shell discovery')
            action(main, 'win.export-extensions')
            export = wait(lambda: window('Backup and Restore'), 'backup window')
            wait(lambda: named(export, 'Include extension settings'), 'settings option')
            assert named(export, 'Include enabled/disabled states')
            for name in ['Deselect All', 'Select All']:
                assert wait(lambda: named(export, name, 'push button') or named(export, name, 'button'), name).do_action(0)
            print('PASS: graphical export discovery/options/selection', flush=True)
            action(export, 'window.close')
            wait(lambda: not window('Backup and Restore'), 'export close')
            action(main, 'win.import-extensions')
            wait(lambda: window('Backup and Restore'), 'import window')
            chooser = wait(lambda: window('Import Extensions'), 'native import chooser')
            action(chooser, 'window.close')
            time.sleep(1)
            imported = window('Backup and Restore')
            assert named(imported, 'Restore extensions')
            assert named(imported, 'Restore extension settings')
            assert named(imported, 'Restore enabled/disabled states')
            print('PASS: graphical import options/native chooser/cancellation', flush=True)
            action(imported, 'window.close')
            action(main, 'window.close')
            process.wait(timeout=15)
            assert process.returncode == 0
        finally:
            if process.poll() is None: process.terminate(); process.wait(timeout=10)
            log.seek(0)
            output = log.read()
            if output: print(output)
