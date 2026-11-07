# Backup format v1

The List format is UTF-8, pretty-printed JSON. The Full format is ZIP containing
exactly `manifest.json` and zero or more `extensions/<UUID>.zip` files. Each inner
ZIP is an ordinary GNOME extension ZIP with root `metadata.json` and `extension.js`,
validated by the existing local ZIP implementation. GNOME's installer installs
code in the user extension directory and compiles schemas. The outer archive is
never extracted into installation or user configuration directories.

Example (abbreviated):

```json
{
  "format": "extension-manager-local",
  "version": 1,
  "shell-version": "51.1",
  "created": "2026-11-07T12:00:00Z",
  "mode": "full",
  "extensions": [{
    "uuid": "example@example.invalid",
    "name": "Example",
    "extension-version": "3",
    "shell-versions": ["51"],
    "source": "user-unknown",
    "enabled": true,
    "bundled": true,
    "settings-skipped": 1,
    "settings-notice": "Unsupported storage and potentially sensitive values omitted",
    "settings": [{
      "schema": "org.gnome.shell.extensions.example",
      "path": "/org/gnome/shell/extensions/example/",
      "values": {
        "opacity": {"type": "i", "value": "75"},
        "shortcut": {"type": "as", "value": "['<Super>a']"}
      }
    }]
  }]
}
```

`type` is a GVariant signature and `value` is GVariant text, preserving exact
types instead of lossy JSON conversion. Defaults and explicit values are saved
for supported keys, to reproduce preferences when defaults change. Schema/path
fields describe the original configuration but **do not authorize writes**.
Import must rediscover the same association from the current installed extension
and check current key/type/range definitions and the privacy filter.

`enabled` is omitted when state export is disabled. `settings` can be empty or
absent. `bundled: true` requires an inner archive with the matching metadata UUID.
Source values currently emitted: `system`, `local` (known pending local install),
and `user-unknown`. Names, homepages and UUIDs do not prove installation provenance.

Readers accept minimal v1 UUID/name records, absent optional fields and unknown
extra fields for additive compatibility. Unknown formats and versions are
rejected; breaking versions require explicit migrations. No earlier backup
format exists in this fork. Unrelated programs' formats are not supported.

## Bounds and security

- Manifest: 8 MiB, 512 unique extensions, 128 schema records per extension,
  2,048 values per schema, 64 KiB per value and bounded signatures.
- Outer input and expanded payload total: 256 MiB; individual bundle: 128 MiB.
  Full output is bounded to 256 MiB. Export smaller groups when necessary.
- Only the precise outer filenames above are accepted. Links, special entries,
  encryption, duplicates, unexpected bundles, invalid UUIDs, truncated payloads,
  missing manifests and malformed JSON are rejected. Actual streamed sizes are
  checked, not just headers. Inner ZIPs reuse the existing hardened central-directory,
  path, link and schema preflight checks and limits.
- Export traverses pinned directory descriptors with `openat(O_NOFOLLOW)` to
  prevent symlink escapes. Limits: depth 32, files 32 MiB each, payload 128 MiB and
  10,000 entries per extension. Hidden/cache directories and compiled schemas are
  excluded. Links, special files and arbitrary binaries are refused; static code,
  UI, images, locales and text assets are supported. JSON files other than root
  `metadata.json` are refused because they may contain runtime credentials.
  Common embedded credential assignments/private-key markers are also refused;
  detection cannot prove arbitrary code is free of secrets. Modes are normalized.
- No backup commands, shell interpolation, system-directory writes, arbitrary
  destinations, whole-dconf dumps or external-file restoration. Inspection never
  executes JavaScript. Installing trusted extension code is a separate operation.
- Known sensitive names are filtered. Arbitrary strings and structured settings
  are omitted. Static code/assets can still embed credentials that cannot be
  reliably recognized in arbitrary source; review custom code before exporting.
  Treat all backups as potentially sensitive. No encryption, signatures or
  authenticity guarantees are supplied.
- Gio private atomic replacement is used where supported. Settings are restored
  per key, not transactionally. Stopping retains completed operations. GNOME's
  existing ZIP installer is also not transactional.

## Development verification

```sh
meson compile -C build
meson test -C build --print-errorlogs
glib-compile-schemas build/data
GSETTINGS_SCHEMA_DIR="$PWD/build/data" \
  python3 tests/backup-gui-smoke.py "$PWD/build/src/extension-manager-local"
```

The backup suite creates private HOME/XDG directories **before** starting its
private D-Bus/dconf service, compiles disposable schemas, writes fixture values,
exports, resets, restores and verifies values/types. It installs a bundled
disposable extension using the real GNOME CLI in private XDG data storage. It
tests changed schemas, removed keys, wrong types/ranges, relocatable paths,
unsupported storage, malformed manifests, v1 additive compatibility, nested
archive attacks, conflicting installations, compatibility and system exclusion.
The existing ZIP suite remains part of Meson testing.

The GUI smoke test needs a live GNOME session, AT-SPI and no running fork instance.
It uses a memory settings backend and isolated XDG directories, exercising
discovery, export controls, import controls and native chooser cancellation.
It does **not** test real desktop preference restoration and is not run headlessly.

Manual end-to-end verification should use a disposable GNOME user/VM and harmless
test extensions: export both formats, remove only disposable extensions, alter
their test preferences, import, inspect results, and log out/in. Repeat with an
unavailable online UUID and an incompatible Shell version. Verify that later
selected extensions still restore. Never use real user preferences as fixtures.
