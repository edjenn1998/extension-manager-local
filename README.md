# Extension Manager Local

A native C/GTK4/libadwaita fork of [Extension Manager](https://github.com/mjakeman/extension-manager), based on commit `c9bc1723340bb58cdf8cffaa09f43f737bf5d4e3` (0.6.5). Original authors: Matthew Jakeman, Óscar Fernández Díaz, and upstream contributors. This is **not an official upstream build**.

This fork adds **local ZIP installation** and **graphical extension backup and restore**. This is the original Extension Manager codebase, not a rewrite or redesign. Browsing, searching, online installation, and the existing management interface come from upstream. The separate name/application ID, purple icon, and AppImage packaging distinguish this build. See [CREDITS.md](CREDITS.md) for attribution.

Changes: **Install from ZIP…**, **Export Extensions…**, **Import Extensions…**, validated local installation/replacement using GNOME's installer, pending-login Installed entries, separate identity, and a purple recoloring of the original icon. Original interface, Installed/Browse tabs, online provider, and management workflows remain. The symbolic icon retains its original monochrome design for GNOME's symbolic rendering. Original artwork and copyright notices remain; see `README.upstream.md` and `COPYING`.

The icon history credits Matthew Jakeman and daudix-UFO. The fork changes only its colors, not its path geometry.

## Run

Download the AppImage from **[GitHub Releases](https://github.com/edjenn1998/extension-manager-local/releases/latest)**. The commands below use the source tree's `dist/` directory; if you downloaded the file elsewhere, omit `dist/`.

```sh
chmod +x dist/ExtensionManagerLocal-x86_64.AppImage
./dist/ExtensionManagerLocal-x86_64.AppImage
```

No root, compilation, development libraries, or Flatpak installation is required to run this actual SquashFS AppImage. Without FUSE, use `./dist/ExtensionManagerLocal-x86_64.AppImage --appimage-extract-and-run`.

Optional GNOME launcher/dock icon integration: `./dist/ExtensionManagerLocal-x86_64.AppImage --install-desktop`. This installs only this fork's desktop entry and purple icon in your user data directory, without root. Keep the AppImage in place, or rerun the command after moving it. On Wayland, this installed desktop ID is how Shell obtains the window/dock icon.

**Host requirements:** x86_64 Linux, glibc ≥2.42, a GNOME Shell session/session D-Bus, `/usr/bin/gnome-extensions`, `/usr/bin/glib-compile-schemas`, Bash, host fonts and CA certificates, and unprivileged user namespaces for sandboxed image decoding. GUI libraries, TLS backend, schemas, translations, icons, image loaders, and Bubblewrap are bundled. Shell, glibc, graphics session, and dconf service remain host components. Tested on CachyOS GNOME 51.1/Wayland; other distributions/older glibc are **not certified**. Generic runtime libraries avoid CachyOS CPU-specific binaries. Flatpak is only a packaging input.

ID: `io.github.edjenn1998.ExtensionManagerLocal`. It can run alongside upstream, with separate application window preferences. **Both apps intentionally manage the same user's GNOME extensions**, not independent collections.

## Install or upgrade

1. Main menu → **Install from ZIP…** → choose a local ZIP.
2. Review name, UUID, version if available, supported Shell versions, and trust warning.
3. Choose **Install** or **Replace**. Identity is determined by UUID, never filename/version. Versionless upgrades and deliberate downgrades are allowed after confirmation.
4. Success means GNOME's installer confirmed installation and the installed UUID was verified, **not** that the new code is running.
5. **Log out and log back in**. Enable a newly installed extension in Installed afterward if wanted.

Opening a local ZIP as an AppImage command-line argument uses the same graphical validation and confirmation flow; it never installs silently or takes over ZIP file associations.

GNOME caches imported JavaScript; even same-version/versionless replacements cannot reliably be hot-reloaded. On Wayland, restarting Shell in place is unavailable. The app does not restart Shell, log you out, disable compatibility checks, or toggle extensions during replacement. Existing enabled/disabled preferences and GSettings/dconf values are left untouched. An extension's changed code/schema can still require migration or stop supporting old settings. System extensions are overridden for your user, never modified system-wide. GNOME's `--force` operation is not transactional: back up important extension code, particularly when disk space is low. Failures show the actual diagnostic and are not reported as successful.

Installed refreshes automatically. New disk-only UUIDs show a login-required description and unavailable toggle until Shell discovers them. Already loaded rows retain the **running Shell version/state**, with a local-update notice during the current app session. After login Shell is authoritative again. Disk-only entries cannot be managed through Shell before discovery.

## Backup and restore

The updated AppImage includes a **Backup and Restore** menu section. Older
local-ZIP-only AppImages must be replaced with the updated build to use it.

### Create a backup

1. Menu → **Export Extensions…**. Select extensions, or use **Select All / Deselect All**.
2. Choose **List Backup (JSON)** or **Full Backup (ZIP with extension code)**.
3. Leave **Include extension settings** and **Include enabled/disabled states**
   checked unless you want to omit them.
4. Choose **Export**, save using the native file dialog, and review the report.
   One extension's failure does not prevent others from being saved. Check the
   report before relying on a backup.

**List Backup** saves UUIDs, names, versions where known, declared Shell versions,
source classification, supported settings and optional states. Missing extensions
are downloaded from extensions.gnome.org during restore; unpublished extensions
cannot be reconstructed from a list alone.

**Full Backup** adds selected user-installed extensions, including unpublished
custom extensions. It does not copy system extensions, protected directories,
external configuration, caches or arbitrary user files. Links, sensitive filenames,
special files and unsupported assets cause that extension's full export to fail
rather than create an incomplete bundle. Review custom code/assets for embedded
credentials before export. Validation is not a code safety audit.
In particular, JSON assets other than `metadata.json` are currently refused
because static data cannot reliably be distinguished from stored credentials.

### Restore after reinstalling Linux

1. Copy your `.json` or `.zip` backup to the new computer (USB, encrypted storage,
   or a secure transfer). There are no machine-specific installation paths.
2. Install this fork, log in to GNOME, and select menu → **Import Extensions…**.
3. Choose the file. Review installed/missing, compatibility and settings status;
   deselect unwanted extensions.
4. Independently select **Restore extensions**, **Restore extension settings**,
   and **Restore enabled/disabled states**. Confirm before settings are overwritten.
   Existing extension files are **kept**, never replaced; use Install from ZIP
   separately if you deliberately want to replace one. No system directories are
   modified.
5. Review the detailed report. A failure does not stop later selections. Stopping
   retains completed operations and skips the rest.
6. Log out/in to load new code. If Shell could not restore a newly installed
   extension's state before login, import again with **only states checked**.
   GNOME's global extensions switch is deliberately not changed.

Example: export a Full Backup of a panel extension and your unpublished extension,
copy it to USB, reinstall Fedora/CachyOS/Ubuntu with a compatible GNOME version,
install this fork, import, confirm settings restoration, review the report, and
log out/in. Bundled extensions do not need to be searched for online.

### Settings coverage and limitations

- Schemas are enumerated from each extension's installed `schemas/gschemas.compiled`,
  **not guessed from UUIDs**. Only `/org/gnome/shell/extensions/` paths are supported.
  Missing compiled schemas are reported; the whole dconf database is never dumped.
- Fixed-path schemas are supported. Relocatable schemas require an unambiguous
  literal constructor in `extension.js` or `prefs.js`, of the form
  `new Gio.Settings({schema_id: 'SCHEMA', path: '/org/gnome/shell/extensions/PATH/'})`.
  Dynamic paths, helper patterns and multiple paths for one schema are not guessed.
- Supported values: booleans, scalar integers/doubles, schema enums, and recognized
  accelerator arrays (`shortcut`, `keybinding`, or `hotkey` keys). Sensitive key
  names are filtered. Arbitrary strings, dictionaries, byte arrays and unknown
  structured values are omitted, even under ordinary-looking keys. Consequently
  some layout preferences, custom categories, sorting lists and other textual
  customization **will not be backed up automatically**.
- No external-file adapters are included yet. Unknown JSON/INI files, databases,
  keyrings, other settings namespaces and unidentified storage are unsupported
  and reported. Do not assume those preferences were preserved.
- Import rediscovers destinations from the **current installed extension** and
  checks schema, path, key, type, range and writability before writing. Incompatible
  preferences are skipped and listed, never forced into dconf. Extension-specific
  migrations may still be necessary.
- GNOME does not reliably retain local-ZIP versus EGO provenance. System and known
  pending local installations are identified; other user installs are honestly
  labeled `user-unknown`.
- Bundled/current installed compatibility uses metadata and the existing Shell
  version matcher. Missing online extensions use the existing EGO provider and
  Shell installer. Offline, removed, incompatible or unverifiable extensions are
  skipped rather than forced. Backup inspection never executes extension code.
- Conservative filtering cannot prove that every ordinary numeric preference or
  embedded code asset is non-sensitive. **Treat backups as potentially sensitive**;
  store and transfer them securely. Backups are not encrypted.
- The format is distribution-independent, but the AppImage's existing glibc/runtime
  requirements still apply. Different GNOME majors may require different code and
  schemas; portability does not guarantee compatibility.

See [docs/backup-format.md](docs/backup-format.md) for format, security limits and tests.

## ZIP validation

Require `metadata.json` and `extension.js` directly at ZIP root, as produced by `gnome-extensions pack`. Require safe `uuid`, `name`, `description`, and nonempty string-array `shell-version`; numeric `version` is optional. Validate a private snapshot before confirmation. Reject absolute/traversing/duplicate/conflicting paths, links, special files, encryption, multi-volume/ZIP64 archives, malformed metadata, and excessive sizes. Limits: 128 MiB compressed, 256 MiB expanded, 1 MiB metadata, 10,000 entries. Preflight schemas with host `glib-compile-schemas --strict --dry-run`. GNOME's CLI, not this fork, extracts into the extension installation directory.

Unsupported Shell versions require an explicit warning/confirmation; GNOME may refuse to load them. GNOME ≥40 matches major version; older versions match major/minor. Validation is not a code safety audit: **only install trusted extension code**.

## Rebuild

Build dependencies: C compiler, Meson, Ninja, pkg-config, gettext, Blueprint compiler, GLib development tools (`gdbus-codegen`, `glib-mkenums`), GTK4, libadwaita ≥1.8, JSON-GLib, libsoup 3, libxml2, Glycin 2/Glycin GTK4 2, and libarchive development files. Tests also use Python 3, GNOME CLI, `dbus-run-session`, GSettings/dconf.

```sh
meson setup build --prefix=/usr -Dbacktrace=false -Dpackage=AppImage
meson compile -C build
meson test -C build --print-errorlogs
python build-aux/package-appimage.py \
  --runtime /path/to/org.gnome.Platform/50/files \
  --dconf /path/to/extracted-generic-x86_64-dconf \
  --dconf-source /path/to/dconf-51.0.tar.xz \
  --appimagetool /path/to/appimagetool.AppImage
```

Install `org.gnome.Platform//50` as a build input, extract a **generic** dconf package, obtain its matching source tarball and [appimagetool](https://github.com/AppImage/appimagetool/releases). Packaging also needs `rsvg-convert`, `gio-querymodules`, `desktop-file-validate`, and ELF utilities. The script checks relocations and creates `dist/ExtensionManagerLocal.AppDir`, the AppImage, and corresponding application source archive. Move an existing generated AppDir aside before rebuilding. AppImage `--check-runtime` prints linking diagnostics; it is not a GUI test. See `PACKAGING.md` and `TEST-RESULTS.md`.

## License

GPL-3.0-or-later (`COPYING`). Retain all copyright/artwork notices. Distribute `ExtensionManagerLocal-source.tar.gz` alongside the binary: it contains the complete modified application source and build scripts. The release's separate `ExtensionManagerLocal-third-party-sources.tar.zst` supplies bundled-library sources and the matching runtime build recipes/patches. Extracting the AppImage allows replacing shared libraries. Bundled library licenses and an exact GNOME runtime source/build manifest are included, with file origins/hashes in `bundled-files.json`. Preserve these source assets when redistributing; see `PACKAGING.md`.
