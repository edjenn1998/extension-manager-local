# Build and verification

## Updated AppImage verification (2026-11-07)

Rebuilt `dist/ExtensionManagerLocal-x86_64.AppImage` from the current working
tree, including graphical backup/restore. Matching application source is in
`dist/ExtensionManagerLocal-source.tar.gz`; release hashes are in `dist/SHA256SUMS`.

- Native build and all five Meson suites pass, including 12 backup and 12 ZIP cases.
- The actual AppImage passes the read-only backup GUI smoke test: Shell discovery,
  export options and selection controls, import options, native chooser opening
  and cancellation, and clean shutdown with fatal GLib criticals enabled.
- AppImage runtime relocation checks pass; bundled sandboxed PNG/SVG decoding and
  icon geometry/color checks pass.
- Rootless desktop-integration tests pass in disposable directories, including
  managed updates, unmanaged-entry protection and special-character launch paths.
- The previous AppImage and AppDir were preserved under
  `/tmp/opencode/exm-previous-package-20261107-092853/`.

These checks did not restore a backup into the live extension collection or
change real extension settings. Complete graphical export/restore, online restore,
and post-login activation are not newly claimed as tested. The user's report that
the previous manual changes worked is not an automated end-to-end test.

## Backup and restore source verification (2026-11-07)

The initial feature verification built natively, before AppImage packaging. Build output:
`/tmp/opencode/exm-build/src/extension-manager-local`. The previous build referred
to missing temporary build tools, so Meson/Blueprint and matching GLib codegen
were provisioned under `/tmp/opencode`; no system packages were changed.

Completed on the local GNOME 51.1 Wayland session:

- Full native application compilation succeeds with no new compiler warnings.
- All five Meson suites pass: desktop, schema and AppStream validation, the
  existing 12 local-ZIP cases, and 12 new backup cases.
- New backup tests cover v1 creation/parsing/additive compatibility, extension
  identity and compatibility, fixed/relocatable schemas, boolean/integer/double/
  enum/accelerator types, read-only restore preview, secret filtering, unsupported
  storage, changed/missing schemas and keys, changed types/ranges, invalid values,
  partial key failures, ZIP round-trip, real GNOME CLI installation in a private
  extension directory, existing conflicts, system-code exclusion, nested path
  traversal, links, malformed/oversized manifests, missing bundles, unsupported
  assets and embedded-credential filtering.
- A real isolated dconf round-trip exports fixture values, resets them and checks
  their exact restored values. HOME/XDG directories and dconf profile are private
  and established before the disposable D-Bus service starts. No live extension
  preferences are used as fixtures.
- All 12 backup cases also pass with AddressSanitizer/UndefinedBehaviorSanitizer.
  Leak detection was disabled; this is not a leak audit.
- Live native GUI smoke passes with memory-backed settings and disposable XDG
  directories: Shell discovery, export options, select/deselect controls, import
  options, native file chooser opening/cancellation, and clean window shutdown.
  `G_DEBUG=fatal-criticals` was enabled. No real extension was installed or toggled
  by the GUI test.

Not claimed as tested: filename submission in the native portal, a complete
graphical restore, real EGO installation/offline behavior during restore,
multi-extension installation failures/cancellation through the GUI, logout/login
activation or state retry, other GNOME versions/distributions, or a newly packaged
AppImage. No AppImage rebuild was performed. Unknown configuration storage,
free-text/dictionary preferences, generalized dynamic relocatable schemas and
external configuration adapters remain unsupported, as documented in README.

Reproduce the new smoke test with the source build's compiled schemas:

```sh
glib-compile-schemas /tmp/opencode/exm-build/data
GSETTINGS_SCHEMA_DIR=/tmp/opencode/exm-build/data \
  python3 tests/backup-gui-smoke.py /tmp/opencode/exm-build/src/extension-manager-local
```

The historical results below describe the previous local-ZIP/AppImage work,
not validation of backup/restore in a release AppImage.

Environment: CachyOS x86_64, GNOME Shell/gnome-extensions 51.1, live Wayland
session. Native compiler GCC 16.2.1, Meson 1.12.1. Upstream base:
`c9bc1723340bb58cdf8cffaa09f43f737bf5d4e3` (0.6.5).

## Completed

- Native C build succeeds, including all original upstream components.
- Meson desktop, AppStream, and generated GSettings schema validation pass.
- All 12 automated ZIP cases pass: metadata and legacy/current compatibility,
  install and versioned/versionless replacement with the real GNOME CLI,
  differently named ZIPs matching by UUID, immutable snapshot behavior,
  duplicate/traversing/absolute/backslash/conflicting paths, links/special files,
  malformed/truncated/wrapped archives, invalid metadata, size/entry limits,
  schema preflight/compilation, actual installer errors, symlink destinations,
  and a separate-filesystem cache.
- A private D-Bus/dconf session confirms extension preferences and enabled/
  disabled UUID lists survive replacements. The live desktop settings were not
  changed by this test.
- Real AppImage built with SquashFS/type2 runtime; executable bit set. Recursive
  ELF relocation checks for the application and all bundled Glycin loaders pass
  with no missing libraries/undefined symbols. Generic, not CachyOS v4, libraries
  are bundled. Older-distribution compatibility is not inferred from this.
- Sandboxed bundled PNG/SVG image decoding passes, including the purple icon
  and an upstream screenshot. The original SVG path geometry, proportions,
  and canvas match exactly. The purple PNG rendition was inspected.
- Optional rootless desktop integration passes in an isolated data directory:
  desktop syntax, icon installation, updating managed entries, protecting
  unmanaged entries, and actual desktop launching with spaces, quotes, percent,
  dollar/backtick/backslash characters in the executable path.
- Live packaged Wayland GUI: existing Shell extension list, native GNOME portal
  picker opening/cancellation, graphical install confirmation, versionless UUID
  replacement confirmation, success/login notice, new/updated Installed entries,
  invalid ZIP error, unsupported-version warning/cancellation, online browsing,
  online Caffeine search, and its detail view through upstream's deep-link path.
  GUI installations used a disposable XDG data directory, not the user's real
  extension directory. Dialog tests submitted files via GApplication's local-file
  open path, sharing exactly the picker callback's validation/install workflow.

The live tests exposed and fixed three packaging problems: host certificate
lookup, host/bundled Fontconfig mismatch, and Glycin Bubblewrap argument ordering.
They also exposed GNOME's cross-filesystem cache-to-data directory move limitation;
the installer now uses a private same-filesystem cache and removes it afterward.

## Explicitly not claimed as tested

- Automated selection of a filename inside GNOME 51's Nautilus portal picker.
  Opening/cancellation passed; its location field was not accessible to the
  automation. Manually select a ZIP using the menu on your desktop.
- Logging out/in to verify Shell discovery and the upgraded code executing.
  The agent did **not** terminate the user's active session. The UI deliberately
  reports installed files separately from activation.
- Enabling/disabling/removing/updating a real user extension, installing a remote
  extension, or changing real extension preferences. These upstream execution
  paths remain intact; existing-list/browsing checks are not a complete runtime
  regression test of every management operation.
- Desktop launcher installation/window-overview icon placement on another system,
  non-English new UI translations, every image format, Shell versions other than
  51.1, other distributions, older glibc, GPU renderer overrides, or GNOME on Xorg.

## Reproduce

```sh
meson compile -C build
meson test -C build --print-errorlogs
python tests/packaged-runtime.py dist/ExtensionManagerLocal.AppDir build/tests/image-driver
/usr/bin/python tests/gui-smoke.py dist/ExtensionManagerLocal-x86_64.AppImage
./dist/ExtensionManagerLocal-x86_64.AppImage --check-runtime
/usr/bin/python tests/desktop-integration.py dist/ExtensionManagerLocal-x86_64.AppImage
```

`gui-smoke.py` needs host PyGObject/AT-SPI, English UI, a live GNOME session,
and no already running fork instance. It creates temporary test evidence in
`/tmp/opencode`, closes only the fork it launched, and does not log out.
