#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Build an AppImage with generic GNOME Platform 50 libraries instead of
CachyOS CPU-optimized host libraries. Requires a built tree and appimagetool."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tarfile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--runtime", type=Path, required=True, help="GNOME Platform 50 files directory")
parser.add_argument("--dconf", type=Path, required=True, help="extracted generic dconf package root")
parser.add_argument("--dconf-source", type=Path, required=True, help="matching dconf source tarball (including COPYING)")
parser.add_argument("--appimagetool", type=Path, required=True)
parser.add_argument("--meson", default="meson")
parser.add_argument("--build", type=Path, default=Path("build"))
args = parser.parse_args()
source = Path(__file__).resolve().parent.parent
os.chdir(source)
dist = source / "dist"
dist.mkdir(exist_ok=True)
appdir = dist / "ExtensionManagerLocal.AppDir"
if appdir.exists():
    raise SystemExit(f"{appdir} already exists; move it aside before rebuilding")
subprocess.run([args.meson, "install", "-C", str(args.build), "--destdir", str(appdir)], check=True)
runtime = args.runtime.resolve()
libraries = appdir / "usr/lib"
libraries.mkdir(parents=True, exist_ok=True)
libdirs = [args.dconf.resolve() / "usr/lib", runtime / "lib/x86_64-linux-gnu", runtime / "lib"]
excluded = {"libc.so.6", "libm.so.6", "libdl.so.2", "libpthread.so.0", "librt.so.1",
            "libresolv.so.2", "libutil.so.1", "ld-linux-x86-64.so.2", "libnss_dns.so.2", "libnss_files.so.2"}
manifest = []
seen = set()


def copy_file(src, dest):
    dest.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(src.resolve(), dest)
    manifest.append(dict(file=str(dest.relative_to(appdir)), source=str(src),
                         sha256=hashlib.sha256(dest.read_bytes()).hexdigest()))


def dependencies(file):
    result = subprocess.check_output(["readelf", "-d", str(file)], text=True)
    for soname in re.findall(r"\(NEEDED\).*\[(.*?)\]", result):
        if soname in excluded or soname in seen:
            continue
        seen.add(soname)
        match = next((directory / soname for directory in libdirs if (directory / soname).exists()), None)
        if match is None:
            raise SystemExit(f"Missing generic runtime dependency: {soname} (needed by {file})")
        copy_file(match, libraries / soname)
        dependencies(match)


dependencies(appdir / "usr/bin/extension-manager-local")
for directory in ["share/icons/Adwaita", "share/icons/AdwaitaLegacy", "share/mime", "share/locale",
                  "share/glycin-loaders", "share/licenses", "etc/fonts"]:
    if (runtime / directory).is_dir():
        shutil.copytree(runtime / directory, appdir / "usr" / directory, dirs_exist_ok=True,
                        symlinks=False, ignore_dangling_symlinks=True)
# Flatpak-specific font paths do not exist in an AppImage. Keep the generic
# font configuration, which already includes the host and user font directories.
for file in (appdir / "usr/etc/fonts/conf.d").glob("*flatpak*"):
    file.unlink()
for file in (runtime / "libexec/glycin-loaders/2+").iterdir():
    copy_file(file, appdir / "usr/libexec/glycin-loaders/2+" / file.name)
    dependencies(file)
copy_file(runtime / "bin/bwrap", appdir / "usr/libexec/bwrap")
dependencies(runtime / "bin/bwrap")
for file in [runtime / "lib/x86_64-linux-gnu/gio/modules/libgiognutls.so",
             args.dconf / "usr/lib/gio/modules/libdconfsettings.so"]:
    copy_file(file, libraries / "gio/modules" / file.name)
    dependencies(file)
subprocess.run(["gio-querymodules", str(libraries / "gio/modules")], check=True)
subprocess.run(["glib-compile-schemas", str(appdir / "usr/share/glib-2.0/schemas")], check=True)
copy_file(source / "build-aux/AppRun", appdir / "AppRun")
copy_file(source / "build-aux/bwrap", appdir / "usr/bin/bwrap")
(appdir / "AppRun").chmod(0o755)
(appdir / "usr/bin/bwrap").chmod(0o755)
app_id = "io.github.edjenn1998.ExtensionManagerLocal"
desktop = appdir / "usr/share/applications" / (app_id + ".desktop")
# AppImages must not activate the fixed /usr/bin D-Bus service.
desktop.write_text(desktop.read_text().replace("DBusActivatable=true", "DBusActivatable=false"))
shutil.copy2(desktop, appdir / desktop.name)
# appimagetool currently looks for the legacy AppStream filename. Keep one
# metadata component, rather than installing two copies with the same ID.
metadata = appdir / "usr/share/metainfo" / (app_id + ".metainfo.xml")
metadata.rename(metadata.with_name(app_id + ".appdata.xml"))
subprocess.run(["appstreamcli", "validate", "--no-net", str(metadata.with_name(app_id + ".appdata.xml"))], check=True)
icon = appdir / "usr/share/icons/hicolor/scalable/apps" / (app_id + ".svg")
shutil.copy2(icon, appdir / icon.name)
png = appdir / (app_id + ".png")
subprocess.run(["rsvg-convert", "-w", "256", "-h", "256", str(icon), "-o", str(png)], check=True)
shutil.copy2(png, appdir / ".DirIcon")
subprocess.run(["desktop-file-validate", str(appdir / desktop.name)], check=True)
licenses = appdir / "usr/share/doc/extension-manager-local"
licenses.mkdir(parents=True, exist_ok=True)
with tarfile.open(args.dconf_source) as dconf_source:
    copying = next(member for member in dconf_source.getmembers()
                   if member.name.count("/") == 1 and member.name.endswith("/COPYING"))
    (licenses / "dconf-COPYING").write_bytes(dconf_source.extractfile(copying).read())
third_party = dist / "third-party-sources"
third_party.mkdir(exist_ok=True)
dconf_source_copy = third_party / args.dconf_source.name
if args.dconf_source.resolve() != dconf_source_copy.resolve():
    shutil.copy2(args.dconf_source, dconf_source_copy)
for name in ["COPYING", "README.md", "PACKAGING.md", "TEST-RESULTS.md", "CREDITS.md", "RELEASE-NOTES.md"]:
    shutil.copy2(source / name, licenses / name)
shutil.copy2(runtime / "manifest.json", licenses / "gnome-runtime-manifest.json")
(licenses / "bundled-files.json").write_text(json.dumps(manifest, indent=2) + "\n")
(dist / "bundled-files.json").write_text(json.dumps(manifest, indent=2) + "\n")
shutil.copy2(runtime / "manifest.json", dist / "gnome-runtime-manifest.json")
shutil.copy2(source / "COPYING", dist / "COPYING")
result = subprocess.run([str(appdir / "AppRun"), "--check-runtime"], text=True, capture_output=True)
(dist / "runtime-link-check.txt").write_text(result.stdout + result.stderr)
if result.returncode or "not found" in result.stdout + result.stderr or "undefined symbol" in result.stdout + result.stderr:
    raise SystemExit("Runtime linking failed; see dist/runtime-link-check.txt")
subprocess.run([str(args.appimagetool.resolve()), "--no-appstream", str(appdir), str(dist / "ExtensionManagerLocal-x86_64.AppImage")],
               env=dict(os.environ, ARCH="x86_64"), check=True)
(dist / "ExtensionManagerLocal-x86_64.AppImage").chmod(0o755)
with tarfile.open(dist / "ExtensionManagerLocal-source.tar.gz", "w:gz") as archive:
    for child in sorted(source.iterdir()):
        if child.name not in {".git", "build", "dist"} and not child.name.startswith("build-"):
            archive.add(child, arcname="ExtensionManagerLocal/" + child.name,
                        filter=lambda info: None if "__pycache__" in info.name else info)
    archive.add(source / "build-aux", arcname="ExtensionManagerLocal/build-aux",
                filter=lambda info: None if "__pycache__" in info.name else info)
