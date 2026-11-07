# Extension Manager Local — backup and restore update

Built directly on **[mjakeman/extension-manager](https://github.com/mjakeman/extension-manager)**. Full credit for the original GNOME-native application and its interface/functionality goes to **Matthew Jakeman, Óscar Fernández Díaz, and the upstream contributors**.

**This build adds graphical extension backup and restore alongside local ZIP installation/replacement.** The Installed/Browse tabs, appearance, online browsing/search/installation, and existing management workflows are retained from upstream. This is not a rewrite or redesign.

The build also has a separate application ID/name and a purple recoloring of the original icon, so it is distinguishable from upstream. Icon geometry is unchanged; upstream icon history credits Matthew Jakeman and daudix-UFO. This is an independent fork, not an official upstream release.

## Download and run

Download `ExtensionManagerLocal-x86_64.AppImage`:

```sh
chmod +x ExtensionManagerLocal-x86_64.AppImage
./ExtensionManagerLocal-x86_64.AppImage
```

Optional rootless launcher and purple dock-icon integration:

```sh
./ExtensionManagerLocal-x86_64.AppImage --install-desktop
```

Open the main menu → **Install from ZIP…**, choose a trusted extension ZIP, and review the installation/replacement confirmation. Existing GSettings/dconf preferences and enabled/disabled preferences are not reset. **Log out and back in to load the installed code**; installation success does not imply hot-reload success.

## Compatibility and verification

Use menu → **Export Extensions…** for a JSON list or a ZIP backup including
supported user-extension code. Use **Import Extensions…** to review and restore
extensions, supported settings, and enabled/disabled states independently.
Existing extension files are kept during restore. Backups are not encrypted;
some settings and assets are deliberately unsupported. Review the operation report
and the README's backup limitations before relying on a backup.

- x86_64 Linux with **glibc 2.42 or newer**, a compatible GNOME Shell session and session D-Bus, host `gnome-extensions` and `glib-compile-schemas`, host fonts/CA certificates, Bash, and unprivileged user namespaces for image sandboxing.
- Tested on **CachyOS, GNOME 51.1, Wayland**. Other distributions and older GNOME versions are not certified.
- Native compilation, 12 ZIP tests, isolated dconf/state-preservation checks, packaged Wayland install/replacement/error dialogs, browsing/search/details, sandboxed image decoding, and rootless desktop-integration tests passed.
- Filename selection inside the native portal picker and code activation after logout/login still require manual verification. Real user extension toggling/removal/remote installation was not exercised. See `TEST-RESULTS.md` for the full limits of testing.

## Source and license

**GPL-3.0-or-later**, with original copyright notices and credits preserved. The release includes the matching modified application source, build scripts, checksums, and third-party source/build information. See `COPYING`, `CREDITS.md`, and `PACKAGING.md`. Base revision: `c9bc1723340bb58cdf8cffaa09f43f737bf5d4e3`; upstream project version: 0.6.5.

`ExtensionManagerLocal-third-party-sources.tar.zst` is a separate, larger source bundle for the included runtime libraries and their build recipes/patches. **It is not needed to run the AppImage.**

Fork-specific issues: https://github.com/edjenn1998/extension-manager-local/issues
