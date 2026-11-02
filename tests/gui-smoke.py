#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Live GNOME/AT-SPI test with isolated XDG_DATA_HOME (English UI).
Requires no running Extension Manager Local. Opens/cancels the native picker;
uses GApplication file opening for subsequent dialog tests, not simulated
selection in the portal's inaccessible filename field. Does not test logout.
"""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time
import gi
import zipfile

gi.require_version("Atspi", "2.0")
from gi.repository import Atspi, GLib

IMAGE = Path(sys.argv[1]).resolve()
ROOT = Path(tempfile.mkdtemp(prefix="exm-gui-", dir="/tmp/opencode"))
UUID = "exm-gui-test@example.invalid"
META = {"uuid": UUID, "name": "Local GUI Test", "description": "Disposable GUI test", "shell-version": ["51"]}
for filename, metadata in [("v1.zip", META), ("v2.zip", dict(META, name="Updated GUI Test")),
                           ("unsupported.zip", dict(META, **{"shell-version": ["40"]}))]:
    with zipfile.ZipFile(ROOT / filename, "w") as archive:
        archive.writestr("metadata.json", json.dumps(metadata))
        archive.writestr("extension.js", "export default class Test { enable() {} disable() {} }")
(ROOT / "invalid.zip").write_text("not a zip")
print("Evidence directory:", ROOT, flush=True)


def children(node):
    for i in range(node.get_child_count()):
        child = node.get_child_at_index(i)
        if child is not None:
            yield child


def tree(node, depth=0):
    if depth > 45:
        return
    yield node
    for child in children(node):
        yield from tree(child, depth + 1)


def wait(function, description, timeout=30):
    until = time.monotonic() + timeout
    while time.monotonic() < until:
        try:
            result = function()
            if result:
                return result
        except GLib.GError:
            pass
        time.sleep(0.2)
    raise AssertionError("Timed out: " + description)


def frame():
    for application in children(Atspi.get_desktop(0)):
        if application.get_name() == "extension-manager-local":
            for window in children(application):
                if window.get_name() == "Extension Manager Local":
                    return window


def named(name, role=None, root=None):
    parent = root or frame()
    if parent:
        return next((n for n in tree(parent) if n.get_name() == name and
                     (role is None or n.get_role_name() == role)), None)


def action(node, name):
    for i in range(node.get_n_actions()):
        if node.get_action_name(i) == name:
            assert node.do_action(i)
            return
    raise AssertionError("Action unavailable: " + name)


def click(name, role="button"):
    def target():
        matches = [n for n in tree(frame()) if n.get_name() == name and n.get_role_name() == role]
        return matches[-1] if matches else None
    assert wait(target, name).do_action(0)
    if name in {"Close", "Cancel"}:
        wait(lambda: not any(n.get_role_name() == "alert" for n in tree(frame())), "dialog dismissal")


def open_zip(filename):
    subprocess.run([str(IMAGE), str(ROOT / filename)], env=environment,
                    stdout=log, stderr=log, check=True, timeout=15)


log = open(ROOT / "app.log", "w")
environment = dict(os.environ, XDG_DATA_HOME=str(ROOT / "data"), LANG="en_US.UTF-8")
process = subprocess.Popen([str(IMAGE)], env=environment, stdout=log, stderr=log)
try:
    wait(frame, "main window")
    wait(lambda: named("User-Installed Extensions"), "existing Shell extension list")
    print("PASS: packaged Wayland GUI and host Shell list", flush=True)
    action(frame(), "win.install-zip")

    def picker():
        for application in children(Atspi.get_desktop(0)):
            for window in children(application):
                if window.get_name() == "Install from ZIP":
                    return window
    chooser = wait(picker, "native file chooser")
    action(chooser, "window.close")
    wait(lambda: any(frame().get_action_name(i) == "win.install-zip"
                     for i in range(frame().get_n_actions())), "picker cancellation")
    print("PASS: native portal picker opening/cancellation (selection not automated)", flush=True)
    open_zip("v1.zip")
    wait(lambda: named("Install Extension?"), "install confirmation")
    assert any(UUID in n.get_name() for n in tree(frame()))
    click("Install")
    wait(lambda: named("Installed — Login Required"), "GNOME installer success")
    click("Close")
    wait(lambda: named("Local GUI Test", "list item"), "automatic Installed refresh")
    destination = ROOT / "data/gnome-shell/extensions" / UUID
    assert (destination / "extension.js").exists()
    print("PASS: metadata, installation, login notice, automatic list refresh", flush=True)
    open_zip("v2.zip")
    wait(lambda: named("Replace Extension?"), "replacement confirmation")
    click("Replace")
    wait(lambda: named("Installed — Login Required"), "replacement success")
    click("Close")
    wait(lambda: named("Updated GUI Test", "list item"), "updated entry")
    assert json.loads((destination / "metadata.json").read_text())["name"] == "Updated GUI Test"
    print("PASS: versionless UUID replacement from a differently named ZIP", flush=True)
    open_zip("invalid.zip")
    wait(lambda: named("Invalid Extension ZIP"), "invalid ZIP error")
    click("Close")
    print("PASS: invalid ZIP diagnostic", flush=True)
    open_zip("unsupported.zip")
    wait(lambda: named("Replace Extension?"), "incompatible replacement warning")
    assert any("does not declare support" in n.get_name() for n in tree(frame()))
    click("Cancel")
    assert json.loads((destination / "metadata.json").read_text())["name"] == "Updated GUI Test"
    print("PASS: incompatible warning and non-destructive cancellation", flush=True)
    click("Browse", "page tab")
    browse = wait(lambda: named("Browse", "panel"), "Browse page")
    wait(lambda: any(n.get_role_name() == "list item" for n in tree(browse)), "online Browse results", 60)
    print("PASS: online Browse results", flush=True)
    search = next(n for n in tree(browse) if n.get_role_name() == "entry")
    assert search.set_text_contents("Caffeine")
    wait(lambda: named("Caffeine", "label", browse), "online search results", 60)
    # GTK does not expose the parameterized ListBoxRow action through AT-SPI.
    # Exercise upstream's existing deep-link path to open its detail view.
    subprocess.run([str(IMAGE), "gnome-extensions://caffeine%40patapon.info"],
                    env=environment, stdout=log, stderr=log, check=True, timeout=15)
    wait(lambda: named("Reviews and Comments", "label"), "online extension details", 60)
    print("PASS: online search and extension detail view", flush=True)
    (ROOT / "accessible-tree.txt").write_text("\n".join(
        n.get_role_name() + ": " + n.get_name() for n in tree(frame()) if n.get_name()))
except Exception:
    if frame():
        dump = "\n".join(n.get_role_name() + ": " + n.get_name() for n in tree(frame()) if n.get_name())
        (ROOT / "failure-tree.txt").write_text(dump)
        print(dump[-8000:], flush=True)
    raise
finally:
    subprocess.run(["gdbus", "call", "--session", "--dest", "io.github.edjenn1998.ExtensionManagerLocal",
                    "--object-path", "/io/github/edjenn1998/ExtensionManagerLocal", "--method",
                    "org.gtk.Actions.Activate", "quit", "[]", "{}"], capture_output=True)
    process.wait(timeout=15)
    log.close()
