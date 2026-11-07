#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Backup tests use a private D-Bus/dconf session and disposable extension tree.
No access to live extension preferences, Shell changes or network is needed.
"""
import copy
import io
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
import zipfile

DRIVER = str(Path(sys.argv[1]).resolve())
UUID = 'backup-test@example.invalid'
FIXED = 'org.gnome.shell.extensions.backup-fixture'
RELOC = FIXED + '.child'
PATH = '/org/gnome/shell/extensions/backup-fixture/child/'

SCHEMAS = '''<schemalist>
 <enum id="org.gnome.shell.extensions.BackupMode"><value nick="compact" value="0"/><value nick="wide" value="1"/></enum>
 <schema id="org.gnome.shell.extensions.backup-fixture" path="/org/gnome/shell/extensions/backup-fixture/">
  <key name="opacity" type="i"><default>40</default><range min="0" max="100"/></key>
  <key name="enabled" type="b"><default>false</default></key>
  <key name="scale" type="d"><default>1.0</default><range min="0.1" max="10.0"/></key>
  <key name="mode" enum="org.gnome.shell.extensions.BackupMode"><default>'compact'</default></key>
  <key name="shortcut" type="as"><default>[]</default></key>
  <key name="ordinary-text" type="s"><default>''</default></key>
  <key name="api-token" type="s"><default>''</default></key>
  <key name="opaque" type="a{ss}"><default>{}</default></key>
 </schema>
 <schema id="org.gnome.shell.extensions.backup-fixture.child">
  <key name="width" type="u"><default>10</default></key>
 </schema>
 <schema id="org.gnome.shell.extensions.backup-fixture.unknown">
  <key name="height" type="i"><default>5</default></key>
 </schema>
</schemalist>'''


class BackupTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory(prefix='exm-backup-', dir='/tmp/opencode')
        cls.root = Path(cls.tmp.name)
        cls.ext = cls.root / 'extension'
        cls.schemas = cls.ext / 'schemas'
        cls.schemas.mkdir(parents=True)
        (cls.schemas / 'test.gschema.xml').write_text(SCHEMAS)
        subprocess.run(['glib-compile-schemas', '--strict', str(cls.schemas)], check=True)
        (cls.ext / 'metadata.json').write_text(json.dumps({'uuid': UUID, 'name': 'Test', 'description': 'Disposable backup fixture', 'shell-version': ['51'], 'version': 3}))
        (cls.ext / 'extension.js').write_text("const settings = new Gio.Settings({schema_id: '" + RELOC + "', path: '" + PATH + "'});\n")
        cls.env = dict(os.environ, GSETTINGS_SCHEMA_DIR=str(cls.schemas), G_DEBUG='fatal-criticals')
        cls.backup = cls.root / 'baseline.json'

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def run_driver(self, *args, ok=True):
        result = subprocess.run([DRIVER, *map(str, args)], env=self.env, text=True, capture_output=True, timeout=40)
        self.assertEqual(result.returncode, 0 if ok else 1, result.stdout + result.stderr)
        return result.stdout

    def settings(self, method, key, value=None, schema=FIXED):
        args = ['gsettings', method, schema, key]
        if value is not None: args.append(value)
        return subprocess.check_output(args, env=self.env, text=True).strip()

    def export(self, full=False):
        filename = self.root / ('backup.zip' if full else 'backup.json')
        self.run_driver('export', self.ext, UUID, filename, 'full' if full else 'list')
        return filename

    def manifest(self):
        return json.loads(self.export().read_text())

    def save(self, obj):
        path = self.root / 'edited.json'
        path.write_text(json.dumps(obj))
        return path

    def test_01_manifest_and_types_roundtrip(self):
        self.settings('set', 'opacity', '73')
        self.settings('set', 'enabled', 'true')
        self.settings('set', 'scale', '2.5')
        self.settings('set', 'mode', 'wide')
        self.settings('set', 'shortcut', "['<Super>a', '<Control>F2']")
        self.settings('set', 'width', 'uint32 27', schema=RELOC + ':' + PATH)
        path = self.export()
        self.assertEqual(path.stat().st_mode & 0o777, 0o600)
        manifest = json.loads(path.read_text())
        self.assertEqual(manifest['version'], 1)
        self.assertEqual(manifest['shell-version'], '51.1')
        self.assertEqual(manifest['extensions'][0]['uuid'], UUID)
        self.assertEqual(manifest['extensions'][0]['shell-versions'], ['51'])
        self.assertFalse(manifest['extensions'][0]['enabled'])
        for key in ['opacity', 'enabled', 'scale', 'mode', 'shortcut']: self.settings('reset', key)
        self.settings('reset', 'width', schema=RELOC + ':' + PATH)
        preview = self.run_driver('check', path, self.ext)
        self.assertIn('6 restorable, 0 skipped', preview)
        self.assertEqual(self.settings('get', 'opacity'), '40')
        report = self.run_driver('restore', path, self.ext)
        self.assertIn('6 restored, 0 skipped', report)
        self.assertEqual(self.settings('get', 'opacity'), '73')
        self.assertEqual(self.settings('get', 'enabled'), 'true')
        self.assertEqual(self.settings('get', 'scale'), '2.5')
        self.assertEqual(self.settings('get', 'mode'), "'wide'")
        self.assertEqual(self.settings('get', 'width', schema=RELOC + ':' + PATH), 'uint32 27')

    def test_02_sensitive_and_unidentified_storage(self):
        self.settings('set', 'ordinary-text', 'secret-disguised-under-ordinary-key')
        self.settings('set', 'api-token', 'very-secret')
        manifest = self.manifest()
        text = json.dumps(manifest)
        self.assertNotIn('very-secret', text)
        self.assertNotIn('secret-disguised', text)
        self.assertNotIn('ordinary-text', text)
        self.assertNotIn('opaque', text)
        self.assertNotIn(FIXED + '.unknown', text)
        self.assertGreaterEqual(manifest['extensions'][0]['settings-skipped'], 4)

    def test_03_changed_key_type_range_and_partial_restore(self):
        obj = self.manifest()
        fixed = next(r for r in obj['extensions'][0]['settings'] if r['schema'] == FIXED)
        fixed['values']['opacity']['value'] = '101'
        fixed['values']['enabled']['type'] = 's'
        fixed['values']['scale']['value'] = 'not a variant'
        fixed['values']['removed'] = {'type': 'i', 'value': '1'}
        self.settings('set', 'opacity', '41')
        report = self.run_driver('restore', self.save(obj), self.ext)
        self.assertIn('4 skipped', report)
        self.assertIn('removed', report)
        self.assertEqual(self.settings('get', 'opacity'), '41')

    def test_04_missing_schema_and_path_change(self):
        obj = self.manifest()
        records = obj['extensions'][0]['settings']
        records[0]['schema'] = 'org.gnome.shell.extensions.gone'
        records[1]['path'] = '/org/gnome/shell/extensions/not-owned/'
        report = self.run_driver('restore', self.save(obj), self.ext)
        self.assertIn('0 restored, 2 skipped', report)

    def test_05_invalid_manifests(self):
        baseline = self.manifest()
        mutations = [None, [], {}, dict(baseline, version=2), dict(baseline, version='1'), dict(baseline, extensions='oops')]
        for uuid in ['../escape', '/absolute', '', '.hidden', 'a\\b', 'a:b']:
            obj = copy.deepcopy(baseline); obj['extensions'][0]['uuid'] = uuid; mutations.append(obj)
        obj = copy.deepcopy(baseline); obj['extensions'] *= 2; mutations.append(obj)
        obj = copy.deepcopy(baseline); obj['extensions'][0]['enabled'] = 'true'; mutations.append(obj)
        obj = copy.deepcopy(baseline); obj['extensions'][0]['settings'][0]['values'] = []; mutations.append(obj)
        obj = copy.deepcopy(baseline); obj['extensions'][0]['settings'][0]['path'] = '/org/gnome/desktop/'; mutations.append(obj)
        for obj in mutations:
            with self.subTest(obj=str(obj)[:80]): self.run_driver('read', self.save(obj), ok=False)
        bad = self.root / 'truncated.json'; bad.write_text('{"format":'); self.run_driver('read', bad, ok=False)

    def test_06_full_archive_and_real_gnome_install(self):
        path = self.export(full=True)
        self.assertEqual(path.stat().st_mode & 0o777, 0o600)
        self.assertIn('1 bundles', self.run_driver('read', path))
        with zipfile.ZipFile(path) as archive:
            self.assertEqual(set(archive.namelist()), {'manifest.json', f'extensions/{UUID}.zip'})
        self.run_driver('install', path, UUID)
        destination = Path(self.env['XDG_DATA_HOME']) / 'gnome-shell/extensions' / UUID
        self.assertTrue((destination / 'extension.js').is_file())
        self.assertTrue((destination / 'schemas/gschemas.compiled').is_file())
        self.run_driver('restore', path, destination)
        # Existing extensions are not silently overwritten.
        self.run_driver('install', path, UUID, ok=False)

    def test_07_archive_attacks(self):
        manifest = json.dumps(self.manifest())
        attacks = ['../outside', '/absolute', 'extensions/../oops.zip', 'extensions/a/b.zip', 'unrelated.txt']
        for name in attacks:
            path = self.root / 'attack.zip'
            with zipfile.ZipFile(path, 'w') as archive:
                archive.writestr('manifest.json', manifest); archive.writestr(name, 'evil')
            self.run_driver('read', path, ok=False)
        path = self.root / 'link.zip'
        with zipfile.ZipFile(path, 'w') as archive:
            archive.writestr('manifest.json', manifest)
            link = zipfile.ZipInfo(f'extensions/{UUID}.zip'); link.create_system = 3; link.external_attr = 0o120777 << 16
            archive.writestr(link, '/etc/passwd')
        self.run_driver('read', path, ok=False)
        with zipfile.ZipFile(path, 'w', compression=zipfile.ZIP_DEFLATED) as archive:
            archive.writestr('manifest.json', ' ' * (8 * 1024 * 1024 + 1))
        self.run_driver('read', path, ok=False)

    def test_08_bundle_identity_and_nested_traversal(self):
        path = self.export(full=True)
        with zipfile.ZipFile(path) as archive: manifest = json.loads(archive.read('manifest.json'))
        nested = io.BytesIO()
        with zipfile.ZipFile(nested, 'w') as archive:
            archive.writestr('../escape', 'evil')
        bad = self.root / 'nested.zip'
        with zipfile.ZipFile(bad, 'w') as archive:
            archive.writestr('manifest.json', json.dumps(manifest))
            archive.writestr(f'extensions/{UUID}.zip', nested.getvalue())
        self.run_driver('read', bad, ok=False)
        with zipfile.ZipFile(bad, 'w') as archive: archive.writestr('manifest.json', json.dumps(manifest))
        self.run_driver('read', bad, ok=False)
        self.run_driver('read', self.save(manifest), ok=False)

    def test_09_symlink_export_and_unsupported_files(self):
        link = self.ext / 'escape.js'
        link.symlink_to('/etc/passwd')
        try: self.run_driver('export', self.ext, UUID, self.root / 'unsafe.zip', 'full', ok=False)
        finally: link.unlink()
        binary = self.ext / 'unsafe.bin'; binary.write_bytes(b'\x00')
        try: self.run_driver('export', self.ext, UUID, self.root / 'unsafe.zip', 'full', ok=False)
        finally: binary.unlink()
        config = self.ext / 'ordinary.json'; config.write_text('{"data":"secret"}')
        try: self.run_driver('export', self.ext, UUID, self.root / 'unsafe.zip', 'full', ok=False)
        finally: config.unlink()
        secret = self.ext / 'ordinary.js'; secret.write_text("const api_key = 'not-a-real-credential';")
        try: self.run_driver('export', self.ext, UUID, self.root / 'unsafe.zip', 'full', ok=False)
        finally: secret.unlink()

    def test_10_minimal_v1_forward_fields_and_no_schema(self):
        obj = {'format': 'extension-manager-local', 'version': 1, 'shell-version': '45',
               'future-field': {'ignored': True}, 'extensions': [{'uuid': UUID, 'name': 'Minimal'}]}
        path = self.save(obj)
        self.run_driver('read', path)
        self.assertIn('no supported settings', self.run_driver('restore', path, self.ext))
        missing = self.root / 'no-schemas'; missing.mkdir(exist_ok=True)
        obj = self.manifest()
        self.assertIn('schema/path unavailable', self.run_driver('restore', self.save(obj), missing))


    def test_11_current_schema_changes(self):
        obj = self.manifest()
        changed = self.root / 'changed'
        schemas = changed / 'schemas'; schemas.mkdir(parents=True, exist_ok=True)
        (schemas / 'test.gschema.xml').write_text(SCHEMAS.replace('name="opacity" type="i"', 'name="opacity" type="u"').replace('name="scale"', 'name="scale-removed"'))
        subprocess.run(['glib-compile-schemas', '--strict', str(schemas)], check=True)
        (changed / 'extension.js').write_text((self.ext / 'extension.js').read_text())
        report = self.run_driver('restore', self.save(obj), changed)
        self.assertIn('2 skipped', report)
        self.assertIn('/opacity:', report)
        self.assertIn('/scale:', report)

    def test_12_discovery_compatibility_and_system_exclusion(self):
        self.assertEqual(self.run_driver('supports', self.ext, UUID, '51.1').strip(), 'known compatible')
        self.assertEqual(self.run_driver('supports', self.ext, UUID, '50.4').strip(), 'known incompatible')
        self.assertEqual(self.run_driver('supports', self.ext, 'wrong@example.invalid', '51').strip(), 'unknown incompatible')
        system = self.root / 'system.zip'
        self.run_driver('export', self.ext, UUID, system, 'system')
        self.assertIn('0 bundles', self.run_driver('read', system))


if __name__ == '__main__':
    if os.environ.get('EXM_BACKUP_PRIVATE_BUS') != '1':
        # dconf-service must inherit these variables at BUS creation, not just
        # the client processes; otherwise service and client use different DBs.
        with tempfile.TemporaryDirectory(prefix='exm-backup-session-', dir='/tmp/opencode') as root:
            profile = Path(root) / 'dconf-profile'
            profile.write_text('user-db:exm_backup_test\n')
            env = dict(os.environ, EXM_BACKUP_PRIVATE_BUS='1', HOME=root,
                       XDG_CONFIG_HOME=root + '/config', XDG_DATA_HOME=root + '/data',
                       XDG_CACHE_HOME=root + '/cache', GIO_USE_VFS='local',
                       GSETTINGS_BACKEND='dconf', DCONF_PROFILE=str(profile))
            env.pop('DCONF_USER_CONFIG_DIR', None)
            raise SystemExit(subprocess.call(['dbus-run-session', '--', sys.executable, __file__, DRIVER], env=env))
    unittest.main(argv=[sys.argv[0]], verbosity=2)
