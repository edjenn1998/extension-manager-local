#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Test generic bundled libraries, sandboxed image decoding and icon geometry."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET

source = Path(__file__).resolve().parent.parent
appdir = Path(sys.argv[1]).resolve()
driver = Path(sys.argv[2]).resolve()
icon = "io.github.edjenn1998.ExtensionManagerLocal.svg"
ns = {"s": "http://www.w3.org/2000/svg"}
original = ET.parse(source / "data/icons/com.mattjakeman.ExtensionManager.svg")
purple = ET.parse(source / "data/icons" / icon)
assert [p.attrib["d"] for p in original.findall("s:path", ns)] == [p.attrib["d"] for p in purple.findall("s:path", ns)]
assert original.getroot().attrib == purple.getroot().attrib
assert "#8b5cf6" in (source / "data/icons" / icon).read_text()
print("PASS: identical icon paths, proportions and canvas; purple color applied")
with tempfile.TemporaryDirectory(prefix="exm-glycin-") as temporary:
    config = Path(temporary) / "glycin-loaders/2+/conf.d"
    config.mkdir(parents=True)
    for file in (appdir / "usr/share/glycin-loaders/2+/conf.d").glob("*.conf"):
        (config / file.name).write_text(file.read_text().replace("Exec=/usr/", "Exec=" + str(appdir / "usr") + "/"))
    env = dict(os.environ, EXM_APPDIR=str(appdir), GLYCIN_DATA_DIR=temporary,
               LD_LIBRARY_PATH=str(appdir / "usr/lib"), PATH=str(appdir / "usr/bin") + ":" + os.environ["PATH"],
               FONTCONFIG_FILE=str(appdir / "usr/etc/fonts/fonts.conf"), FONTCONFIG_PATH=str(appdir / "usr/etc/fonts"))
    for file in [appdir / ".DirIcon", appdir / icon, source / "data/screenshot-installed-light.png"]:
        subprocess.run([str(driver), str(file)], env=env, check=True, timeout=60)
    print("PASS: bundled PNG/SVG decoders with Bubblewrap sandbox enabled")
