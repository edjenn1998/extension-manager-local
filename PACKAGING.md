# Portable build notes

Generic x86_64 libraries: `org.gnome.Platform//50`, commit
`4337a389125f814785bd83f4ace01da5b6baa80fdcf5a9c81e60dcd03896c067`.
GTK 4.22.5, libadwaita 1.9.4, GLib 2.88.4, Glycin 2.1.5. The native build
used GTK 4.24.1/libadwaita 1.10.0/GLib 2.90.1; relocation checks against the
bundled older ABI are mandatory. glibc is not bundled; baseline is 2.42.
The tested host has glibc 2.44. No host CachyOS v4 libraries are bundled.

Generic dconf 51.0-1:
https://archlinux.cachyos.org/repo/extra/os/x86_64/dconf-51.0-1-x86_64.pkg.tar.zst
Source: https://download.gnome.org/sources/dconf/51/dconf-51.0.tar.xz
Recipe: https://gitlab.archlinux.org/archlinux/packaging/packages/dconf
(tag `51.0-1`). Use the host dconf daemon/session activation service.
The matching dconf tarball is included in `dist/third-party-sources/`, SHA-256
`e65c1b7867f836faad9f9ee04acf5c5a6cae2bbc784833fe093207e0cb590248`.

Builder: https://github.com/AppImage/appimagetool continuous commit `4919cc4`
(2026-11-01). The builder downloads/embeds type2-runtime:
https://github.com/AppImage/type2-runtime , embedded runtime commit `fedfad5`.
Record the final artifact hash.

## Runtime integration

- Recursively resolve ELF DT_NEEDED only from the generic runtime. Never silently
  fall back to host GUI libraries. Check the application and all image loaders
  with `ldd -r`.
- Bundle GIO TLS, selecting host maintained PEM CA certificates using
  `GTlsFileDatabase`, rather than the runtime's compiled-in PKCS#11 path.
  HTTPS certificate checks remain enabled.
- Bundle matching Fontconfig configuration; retain host fonts.
- Default to GTK Cairo to avoid Vulkan/OpenGL driver ABI mixing. Widgets/styles
  are unchanged. A user `GSK_RENDERER` override is at their compatibility risk.
- Generate private per-run Glycin configuration. Bubblewrap adds a read-only
  AppImage bind and matching library path; namespace/seccomp sandboxing remains
  enabled. Host user namespaces must be permitted.
- Restore host library/module/schema/PATH environment before executing GNOME's
  supported CLI installer or schema compiler.
- Disable portable desktop D-Bus activation because `/usr/bin/extension-manager-local`
  is not a relocatable path. For optional desktop integration, copy the included
  desktop/icon files to `~/.local/share/applications` and `~/.local/share/icons`,
  change `Exec` to the absolute AppImage path, and keep `DBusActivatable=false`.
  `AppImage --install-desktop` performs this rootless integration on explicit
  request; plain launch does not write desktop integration files. It refuses to
  overwrite an unmanaged existing entry with the fork's ID.

The original `build-aux/com.mattjakeman.ExtensionManager*.json` Flatpak recipes
are retained as upstream references, not supported fork installers. They should
not be used to distribute this fork; no Flatpak fallback was needed or built.

## Third-party source availability

The release provides **`ExtensionManagerLocal-third-party-sources.tar.zst`**
alongside the AppImage and matching application source archive. This separate
bundle is large because it includes the libraries' upstream sources and Rust
dependency sources, not just links to their homepages.

`dist/gnome-runtime-manifest.json` records upstream tarballs/checksums and Git
revisions. `build-aux/collect-release-sources.py` downloads the selected bundled
components, verifies archive checksums, and obtains Git snapshots pinned to
exact commits. Its output contains `runtime/modules.json` and
`runtime/sources.json`, mapping every downloaded archive to its module, URL,
revision, and SHA-256. Source filenames ending in `.source` retain the original
archive encoding; `file`/`tar` can identify them.

Exact runtime build recipes, local files, and patch queues are included:

- GNOME build metadata revision **`0ed65ce92`**, identified by the installed
  runtime's OSTree commit subject (`50.6-18-g0ed65ce92`).
- Freedesktop SDK revision **`a32cb6d7b91a8e6089224119457d5cb40d17e1a5`**,
  referenced by that GNOME build metadata, including GNOME's SDK patch queue.
- Arch dconf packaging tag **`51.0-1`**, plus the dconf 51.0 source archive.
- AppImage type2-runtime revision **`fedfad51acc2860b2916a1eef32ba4a500ff31ef`**
  and its pinned libfuse 3.15.0 / Squashfuse 0.5.2 source dependencies. The
  runtime recipe supplies the libfuse patch and static-link build commands.

All source-bundle files have a checksum inventory. License texts remain in the
AppImage and source archives. If redistributing this binary, preserve these
source assets and notices alongside it; do not substitute a link to upstream
for the modified application's source. Shared libraries can be replaced by
extracting the AppImage. This fork does not modify the bundled libraries.
