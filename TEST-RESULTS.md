# Build and verification

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
