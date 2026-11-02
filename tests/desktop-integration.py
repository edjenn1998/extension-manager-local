#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Exercise opt-in desktop integration without modifying the real desktop."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time
import gi

gi.require_version("Gio", "2.0")
from gi.repository import Gio, GLib

image = Path(sys.argv[1]).resolve()
with tempfile.TemporaryDirectory(prefix="exm-desktop-", dir="/tmp/opencode") as temporary:
    data = Path(temporary)
    env = dict(os.environ, XDG_DATA_HOME=temporary)
    subprocess.run([str(image), "--install-desktop"], env=env, check=True)
    desktop = data / "applications/io.github.edjenn1998.ExtensionManagerLocal.desktop"
    subprocess.run(["desktop-file-validate", str(desktop)], check=True)
    info = Gio.DesktopAppInfo.new_from_filename(str(desktop))
    assert info is not None
    parsed, argv = GLib.shell_parse_argv(info.get_commandline())
    assert parsed and argv[:2] == ["/usr/bin/env", str(image)], info.get_commandline()
    assert (data / "icons/hicolor/scalable/apps/io.github.edjenn1998.ExtensionManagerLocal.svg").exists()
    subprocess.run([str(image), "--install-desktop"], env=env, check=True)
    desktop.write_text("Unmanaged custom entry\n")
    result = subprocess.run([str(image), "--install-desktop"], env=env, capture_output=True, text=True)
    assert result.returncode != 0 and "Refusing to overwrite" in result.stderr
    assert desktop.read_text() == "Unmanaged custom entry\n"
    print("PASS: rootless launcher/icon installation, update, and unmanaged-entry protection")

    # Exercise the desktop launch parser, not just metadata syntax, with paths
    # requiring quoting. The substitute executable only writes a temp marker.
    stub = data / 'portable 100% "quoted" $dollar `tick` back\\slash.AppImage'
    stub.write_text('#!/bin/sh\nprintf done > "$EXM_DESKTOP_TEST_OUTPUT"\n')
    stub.chmod(0o755)
    private_data = data / "quoted-data"
    appdir_run = image.parent / "ExtensionManagerLocal.AppDir/AppRun"
    subprocess.run([str(appdir_run), "--install-desktop"],
                    env=dict(env, APPIMAGE=str(stub), XDG_DATA_HOME=str(private_data)), check=True)
    quoted = private_data / "applications/io.github.edjenn1998.ExtensionManagerLocal.desktop"
    subprocess.run(["desktop-file-validate", str(quoted)], check=True)
    marker = data / "launched"
    os.environ["EXM_DESKTOP_TEST_OUTPUT"] = str(marker)
    assert Gio.DesktopAppInfo.new_from_filename(str(quoted)).launch([], None)
    until = time.monotonic() + 5
    while not marker.exists() and time.monotonic() < until:
        time.sleep(0.05)
    assert marker.read_text() == "done"
    print("PASS: desktop launch handles spaces, quotes, percent and shell-special path characters")
