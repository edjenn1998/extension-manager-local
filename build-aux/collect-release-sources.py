#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Collect pinned upstream sources and runtime build recipes for release assets.

Archives with manifest checksums are verified. Git snapshots use exact commits,
not moving tags. No downloaded source code is executed.
"""
import concurrent.futures
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tarfile
import urllib.parse

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / "dist/third-party-sources/runtime"
CACHE = Path("/tmp/opencode/exm-release-source-cache")
OUT.mkdir(parents=True, exist_ok=True)
CACHE.mkdir(parents=True, exist_ok=True)
manifest = json.loads((ROOT / "dist/gnome-runtime-manifest.json").read_text())
stems = set('''gcc-base gcc-libs libffi bzip2 zlib xz zstd pcre2 attr acl
libselinux libsepol libeconf util-linux-base util-linux glib libcap libunwind
systemd-base systemd-libs dbus-base libdbus icu libxml2 xorg-lib-x11 xorg-lib-xcb
xorg-lib-xau xorg-lib-xext xorg-lib-xrender xorg-lib-xi xorg-lib-xcursor
xorg-lib-xfixes xorg-lib-xdamage xorg-lib-xrandr xorg-lib-xinerama
brotli libpng wayland graphite2 harfbuzz freetype fontconfig lzo pixman cairo
gdk-pixbuf libglycin glycin libglycin-gtk4 json-glib e2fsprogs keyutils krb5
avahi-libs cups-base cups libglvnd libepoxy jpeg libwebp libtiff shared-mime-info
lcms libseccomp bubblewrap fribidi libdatrie libthai pango aom dav1d openjpeg
svt-av1 libheif highway libjxl librsvg libnghttp2 libidn2 libunistring libpsl
gmp nettle libtasn1 p11-kit gnutls curl glib-networking libsoup openssl sqlite
elfutils gstreamer gstreamer-plugins-base vulkan-icd-loader graphene orc
libdrm gtk adwaita-icon-theme hicolor-icon-theme libadwaita libarchive libyaml
libxmlb appstream expat libxkbcommon'''.split())
modules = [m for m in manifest["modules"] if Path(m["name"]).stem in stems]
seen_names = {Path(m["name"]).stem for m in modules}
assert stems <= seen_names, stems - seen_names

# Avahi is a filtered artifact whose base source is omitted by the generated
# runtime manifest. Its exact source is specified by the matching SDK recipe.
modules.append({"name": "components/avahi-base.bst", "sources": [{"type": "git",
    "url": "https://github.com/lathiat/avahi.git",
    "commit": "f060abee2807c943821d88839c013ce15db17b58"}]})

mirrors = {
    "https://gcc.gnu.org/git/gcc": "https://github.com/gcc-mirror/gcc",
    "https://gitlab.freedesktop.org/fontconfig/fontconfig": "https://github.com/mirror/fontconfig",
    "https://gitlab.freedesktop.org/gstreamer/gstreamer": "https://github.com/GStreamer/gstreamer",
    "https://sourceware.org/git/bzip2": "https://github.com/libarchive/bzip2",
    "https://git.savannah.gnu.org/git/gnulib": "https://github.com/coreutils/gnulib",
    "https://git.kernel.org/pub/scm/utils/util-linux/util-linux": "https://github.com/util-linux/util-linux",
    "https://chromium.googlesource.com/webm/libwebp": "https://github.com/webmproject/libwebp",
    "https://code.videolan.org/videolan/dav1d": "https://github.com/videolan/dav1d",
    "https://git.lysator.liu.se/nettle/nettle": "https://github.com/gnutls/nettle",
}


def urls_for(source):
    url = source["url"]
    if source["type"] == "archive":
        return [url]
    sha = re.search(r"([0-9a-f]{40})$", source["commit"]).group(1)
    base = url.removesuffix(".git").rstrip("/")
    bases = [base]
    if base in mirrors:
        bases.insert(0, mirrors[base])
    urls = []
    for base in bases:
        parsed = urllib.parse.urlparse(base)
        if parsed.netloc == "github.com":
            urls.append(f"https://codeload.github.com{parsed.path}/tar.gz/{sha}")
        elif "gitlab" in parsed.netloc:
            name = parsed.path.rsplit("/", 1)[-1]
            urls.append(f"{base}/-/archive/{sha}/{name}-{sha}.tar.gz")
        elif parsed.netloc == "git.savannah.gnu.org":
            # Savannah exposes snapshot archives through cgit.
            name = parsed.path.rsplit("/", 1)[-1]
            urls.append(f"https://cgit.git.savannah.gnu.org/cgit/{name}.git/snapshot/{name}-{sha}.tar.gz")
        elif parsed.netloc == "git.kernel.org":
            name = parsed.path.rsplit("/", 1)[-1]
            urls.append(f"{base}.git/snapshot/{name}-{sha}.tar.gz")
        elif parsed.netloc.endswith("googlesource.com"):
            urls.append(f"{base}/+archive/{sha}.tar.gz")
    return urls


sources = {}
for module in modules:
    for source in module.get("sources", []):
        if source["type"] not in {"archive", "git"}:
            continue  # patches/local files are in the exact recipe snapshots
        identity = json.dumps({k: source.get(k) for k in ["type", "url", "sha256", "commit"]}, sort_keys=True)
        key = hashlib.sha256(identity.encode()).hexdigest()
        if key not in sources:
            sources[key] = dict(source, modules=[])
        if module["name"] not in sources[key]["modules"]:
            sources[key]["modules"].append(module["name"])


def download(item):
    key, source = item
    candidates = urls_for(source)
    name = key + (".crate" if source["url"].endswith(".crate") else ".source")
    cached = CACHE / name
    expected = source.get("sha256")
    chosen = None
    if cached.exists():
        if not expected or hashlib.sha256(cached.read_bytes()).hexdigest() == expected:
            chosen = "cache"
    if chosen is None:
        errors = []
        for url in candidates:
            temporary = cached.with_suffix(".download")
            result = subprocess.run(["curl", "--location", "--fail", "--silent", "--show-error",
                                     "--retry", "2", "--max-time", "240", "--output", str(temporary), url],
                                    capture_output=True, text=True)
            if result.returncode:
                errors.append(url + ": " + result.stderr.strip())
                continue
            digest = hashlib.sha256(temporary.read_bytes()).hexdigest()
            if expected and digest != expected:
                errors.append(url + ": SHA-256 mismatch")
                continue
            # Avoid silently including an HTML login/error page as Git source.
            if source["type"] == "git" and not tarfile.is_tarfile(temporary):
                errors.append(url + ": not a source archive")
                continue
            temporary.replace(cached)
            chosen = url
            break
        if chosen is None:
            if source["type"] != "git":
                raise RuntimeError("\n".join(errors))
            sha = re.search(r"([0-9a-f]{40})$", source["commit"]).group(1)
            repository = CACHE / (key + ".git")
            env = dict(os.environ, GIT_TERMINAL_PROMPT="0")
            subprocess.run(["git", "init", "--bare", str(repository)], check=True, capture_output=True)
            result = subprocess.run(["git", "-C", str(repository), "fetch", "--depth=1", source["url"], sha],
                                    env=env, capture_output=True, text=True, timeout=240)
            if result.returncode:
                raise RuntimeError("\n".join(errors) + "\nGit fetch: " + result.stderr)
            fetched = subprocess.check_output(["git", "-C", str(repository), "rev-parse", "FETCH_HEAD"], text=True).strip()
            if fetched != sha:
                raise RuntimeError("Git fetch returned a different commit")
            subprocess.run(["git", "-C", str(repository), "archive", "--format=tar.gz",
                            "--prefix=source/", "--output=" + str(cached), sha], check=True)
            chosen = "git fetch " + source["url"] + " " + sha
    destination = OUT / name
    shutil.copy2(cached, destination)
    return dict(source, file=name, sha256=hashlib.sha256(destination.read_bytes()).hexdigest(),
                downloaded_from=chosen, candidate_urls=candidates)


(OUT / "modules.json").write_text(json.dumps(modules, indent=2) + "\n")
records, errors = [], []
with concurrent.futures.ThreadPoolExecutor(max_workers=8) as executor:
    futures = {executor.submit(download, item): item for item in sources.items()}
    for future in concurrent.futures.as_completed(futures):
        try:
            result = future.result()
            records.append(result)
            print("Fetched", result["url"], flush=True)
        except Exception as error:
            item = futures[future]
            errors.append(dict(source=item[1], error=str(error)))
            print("FAILED", item[1]["url"], str(error), flush=True)
(OUT / "sources.json").write_text(json.dumps(sorted(records, key=lambda r: r["file"]), indent=2) + "\n")
(OUT / "errors.json").write_text(json.dumps(errors, indent=2) + "\n")
print(f"Collected {len(records)} sources; {len(errors)} failures", flush=True)
if errors:
    raise SystemExit(1)
