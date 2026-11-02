#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Tests use a temporary XDG data directory, never real installed extensions."""
import json
import os
from pathlib import Path
import shutil
import stat
import subprocess
import sys
import tempfile
import unittest
import warnings
import zipfile

DRIVER = str(Path(sys.argv.pop(1)).resolve())
UUID = "exm-local-test@example.invalid"
METADATA = dict(uuid=UUID, name="Local ZIP Test", description="Disposable test", **{"shell-version": ["51"]})
EXTENSION = b"export default class Test { enable() {} disable() {} }\n"
SCHEMA = '''<schemalist><schema id="io.github.edjenn1998.ExmLocalTest" path="/org/gnome/shell/extensions/exm-local-test/">
<key name="value" type="s"><default>'default'</default></key></schema></schemalist>'''


class LocalZipTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="exm-test-")
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.env = dict(os.environ, XDG_DATA_HOME=str(self.root / "data"),
                        XDG_CACHE_HOME=str(self.root / "cache"), XDG_CONFIG_HOME=str(self.root / "config"))
        (self.root / "cache").mkdir()
        self.destination = self.root / "data/gnome-shell/extensions" / UUID

    def bundle(self, name="test.zip", metadata=None, entries=None):
        path = self.root / name
        with warnings.catch_warnings(), zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as archive:
            warnings.simplefilter("ignore", UserWarning)
            archive.writestr("metadata.json", json.dumps(METADATA if metadata is None else metadata))
            archive.writestr("extension.js", EXTENSION)
            for key, value in entries or []:
                archive.writestr(key, value)
        return path

    def run_driver(self, command, path, *args, ok=True, env=None):
        result = subprocess.run([DRIVER, command, str(path), *args], env=env or self.env,
                                text=True, capture_output=True, timeout=60)
        self.assertEqual(result.returncode == 0, ok, result.stdout + result.stderr)
        self.assertNotIn("CRITICAL", result.stderr)
        return result

    def test_metadata_and_compatibility(self):
        path = self.bundle()
        result = self.run_driver("inspect", path)
        self.assertIn(UUID, result.stdout)
        self.assertIn("no version", result.stdout)
        self.run_driver("supports", path, "51.1")
        self.run_driver("supports", path, "50.5", ok=False)
        self.run_driver("supports", self.bundle("legacy.zip", dict(METADATA, **{"shell-version": ["3.38"]})), "3.38.4")

    def test_install_replace_without_version_and_snapshot(self):
        path = self.bundle()
        self.run_driver("snapshot", path)
        self.assertEqual((self.destination / "extension.js").read_bytes(), EXTENSION)
        updated = self.bundle("different-filename.zip", dict(METADATA, name="Updated"), [("new.txt", "new")])
        self.run_driver("install", updated, ok=False)
        self.run_driver("replace", updated)
        self.assertEqual(json.loads((self.destination / "metadata.json").read_text())["name"], "Updated")
        self.assertTrue((self.destination / "new.txt").exists())

    def test_versioned_upgrade(self):
        self.run_driver("install", self.bundle(metadata=dict(METADATA, version=1)))
        self.run_driver("replace", self.bundle("v2.zip", dict(METADATA, version=2)))
        self.assertEqual(json.loads((self.destination / "metadata.json").read_text())["version"], 2)

    def test_cache_on_another_filesystem(self):
        # The installer gets a private cache beside XDG_DATA_HOME, avoiding
        # GNOME's cross-filesystem GFile move limitation.
        with tempfile.TemporaryDirectory(prefix="exm-cache-", dir="/dev/shm") as cache:
            env = dict(self.env, XDG_CACHE_HOME=cache)
            self.run_driver("install", self.bundle(), env=env)
            self.assertFalse(list(Path(cache).iterdir()))
            self.assertFalse(list((self.root / "data").glob(".extension-manager-local-*")))

    def test_unsafe_paths_and_duplicates(self):
        for key in ["../escape", "/tmp/escape", "a/../../escape", "a\\escape", "C:/escape", "a//b", "./extension.js", "metadata.json", "a\nb"]:
            with self.subTest(key=key):
                self.run_driver("inspect", self.bundle(entries=[(key, "bad")]), ok=False)
        self.run_driver("inspect", self.bundle(entries=[("a", "file"), ("a/b", "conflict")]), ok=False)

    def test_links_and_special_files(self):
        for mode in [stat.S_IFLNK, stat.S_IFIFO, stat.S_IFCHR]:
            entry = zipfile.ZipInfo("bad")
            entry.create_system = 3
            entry.external_attr = (mode | 0o777) << 16
            self.run_driver("inspect", self.bundle(entries=[(entry, "../outside")]), ok=False)

    def test_invalid_metadata(self):
        cases = [[], None, dict(METADATA, uuid="../../outside"), dict(METADATA, uuid="."),
                 dict(METADATA, name=3), dict(METADATA, version="two"),
                 dict(METADATA, **{"shell-version": []}), dict(METADATA, **{"shell-version": [51]}),
                 dict(METADATA, **{"shell-version": "51"}), dict(METADATA, **{"shell-version": ["not-a-version"]})]
        for case in cases:
            with self.subTest(case=case):
                path = self.root / "bad.zip"
                with zipfile.ZipFile(path, "w") as archive:
                    archive.writestr("metadata.json", json.dumps(case))
                    archive.writestr("extension.js", EXTENSION)
                self.run_driver("inspect", path, ok=False)

    def test_invalid_archives_and_wrapped_structure(self):
        path = self.root / "bad.zip"
        for contents in [b"not a zip", b"PK\x03\x04truncated"]:
            path.write_bytes(contents)
            self.run_driver("inspect", path, ok=False)
        with zipfile.ZipFile(path, "w") as archive:
            archive.writestr("wrapper/metadata.json", json.dumps(METADATA))
            archive.writestr("wrapper/extension.js", EXTENSION)
        self.run_driver("inspect", path, ok=False)

    def test_schema_preflight_and_compile(self):
        path = self.bundle(entries=[("schemas/test.gschema.xml", SCHEMA)])
        self.run_driver("install", path)
        self.assertTrue((self.destination / "schemas/gschemas.compiled").exists())
        bad = self.bundle("bad-schema.zip", entries=[("schemas/test.gschema.xml", "<not-a-schema>")])
        self.run_driver("replace", bad, ok=False)
        self.assertTrue((self.destination / "schemas/gschemas.compiled").exists())

    def test_real_installer_failure_reason_and_symlink(self):
        base = self.root / "data/gnome-shell"
        base.mkdir(parents=True)
        (base / "extensions").write_text("not a directory")
        result = self.run_driver("install", self.bundle(), ok=False)
        self.assertIn("gnome-extensions", result.stderr)
        (base / "extensions").unlink()
        (base / "extensions").symlink_to(self.root / "elsewhere")
        result = self.run_driver("install", self.bundle(), ok=False)
        self.assertIn("symlink", result.stderr)

    def test_size_limits(self):
        self.run_driver("inspect", self.bundle(entries=[("too-large", bytes(257 * 1024 * 1024))]), ok=False)
        self.run_driver("inspect", self.bundle(entries=[("many/" + str(i), "") for i in range(10000)]), ok=False)

    def test_dconf_and_enabled_preferences_survive(self):
        schemas = self.root / "schemas"
        schemas.mkdir()
        (schemas / "test.gschema.xml").write_text(SCHEMA)
        subprocess.run(["glib-compile-schemas", str(schemas)], check=True)
        path = self.bundle(entries=[("schemas/test.gschema.xml", SCHEMA)])
        updated = self.bundle("v2.zip", dict(METADATA, version=2), [("schemas/test.gschema.xml", SCHEMA)])
        env = dict(self.env, GSETTINGS_SCHEMA_DIR=str(schemas))
        # A private bus ensures dconf's service inherits this temporary config
        # directory. No settings in the live desktop session are changed.
        result = subprocess.run(["dbus-run-session", "--", sys.executable, __file__, DRIVER,
                                 "--settings-child", str(path), str(updated)], env=env,
                                text=True, capture_output=True, timeout=60)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


def settings_child():
    def setting(*args):
        return subprocess.check_output(["gsettings", *args], text=True).strip()
    setting("set", "io.github.edjenn1998.ExmLocalTest", "value", "'keep me'")
    setting("set", "org.gnome.shell", "enabled-extensions", "['" + UUID + "']")
    before = setting("get", "org.gnome.shell", "enabled-extensions")
    subprocess.run([DRIVER, "install", sys.argv[2]], check=True)
    subprocess.run([DRIVER, "replace", sys.argv[3]], check=True)
    assert setting("get", "io.github.edjenn1998.ExmLocalTest", "value") == "'keep me'"
    assert setting("get", "org.gnome.shell", "enabled-extensions") == before
    setting("set", "org.gnome.shell", "disabled-extensions", "['" + UUID + "']")
    disabled = setting("get", "org.gnome.shell", "disabled-extensions")
    subprocess.run([DRIVER, "replace", sys.argv[3]], check=True)
    assert setting("get", "org.gnome.shell", "disabled-extensions") == disabled


if __name__ == "__main__":
    if len(sys.argv) > 1 and sys.argv[1] == "--settings-child":
        settings_child()
    else:
        unittest.main(verbosity=2)
