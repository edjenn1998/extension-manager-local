# Extension Manager Local

A native C/GTK4/libadwaita fork of [Extension Manager](https://github.com/mjakeman/extension-manager), based on commit `c9bc1723340bb58cdf8cffaa09f43f737bf5d4e3` (0.6.5). Original authors: Matthew Jakeman, Óscar Fernández Díaz, and upstream contributors. This is **not an official upstream build**.

**The only added extension-management feature is installing extensions from local ZIP files, including replacing an existing extension with an updated ZIP.** This is the original Extension Manager codebase, not a rewrite or redesign. Browsing, searching, online installation, and the existing management interface come from upstream. The separate name/application ID, purple icon, and AppImage packaging distinguish this build; they are not additional management features. See [CREDITS.md](CREDITS.md) for attribution.

Changes: one **Install from ZIP…** menu action, validated local installation/replacement using GNOME's installer, pending-login Installed entries, separate identity, and a purple recoloring of the original icon. Original interface, Installed/Browse tabs, online provider, and management workflows remain. The symbolic icon retains its original monochrome design for GNOME's symbolic rendering. Original artwork and copyright notices remain; see `README.upstream.md` and `COPYING`.

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

## Validation

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
