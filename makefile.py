#!/usr/bin/env python3
from __future__ import annotations

import os
import shlex
import shutil
import subprocess
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parent
BUILD = Path(os.environ.get("GNUCHAN_PLYMOUTH_BUILD") or ROOT / "_build")
DESTDIR = Path(os.environ.get("DESTDIR") or "/")
PREFIX = Path(os.environ.get("PREFIX") or "/usr/local")
ASSETS = Path(os.environ.get("GNUCHANOS_ASSETS", ROOT / "assets"))
if not (ASSETS / "bg.png").is_file() or not (ASSETS / "logo.png").is_file():
    ASSETS = ROOT / "assets"


def build() -> Path:
    if not (ASSETS / "bg.png").is_file() or not (ASSETS / "logo.png").is_file():
        raise SystemExit(
            "GnuChanPlymouth requires bg.png and logo.png in GNUCHANOS_ASSETS "
            "or the repository's assets directory"
        )
    BUILD.mkdir(parents=True, exist_ok=True)
    binary = BUILD / "GnuChanPlymouth"
    drm_flags = shlex.split(
        subprocess.check_output(["pkg-config", "--cflags", "--libs", "libdrm"],
                                text=True)
    )
    subprocess.run(
        [
            os.environ.get("CC", "cc"),
            "-std=c11",
            "-O2",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-pedantic",
            str(ROOT / "src" / "gnuchan_plymouth.c"),
            "-o",
            str(binary),
            "-lpng",
            *drm_flags,
        ],
        check=True,
    )
    return binary


def install_file(source: Path, destination: Path, mode: int = 0o644) -> None:
    target = DESTDIR / destination.relative_to("/")
    target.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(source, target)
    target.chmod(mode)


def install() -> None:
    binary = build()
    install_file(binary, PREFIX / "bin" / "GnuChanPlymouth", 0o755)
    install_file(ASSETS / "bg.png", Path("/usr/share/gnuchan-plymouth/background.png"))
    install_file(ASSETS / "logo.png", Path("/usr/share/gnuchan-plymouth/logo.png"))

    profiles = ROOT / "src" / "config"
    for profile in ("intel.conf", "amd.conf", "nvidia.conf"):
        install_file(profiles / profile,
                     Path("/etc/gnuchan-plymouth") / profile)

    install_file(
        ROOT / "systemd" / "gnuchan-plymouth.service",
        Path("/usr/lib/systemd/system/gnuchan-plymouth.service"),
    )
    install_file(
        ROOT / "systemd" / "gnuchandm.service.d" / "gnuchan-plymouth.conf",
        Path("/etc/systemd/system/gnuchandm.service.d/gnuchan-plymouth.conf"),
    )


def main() -> None:
    command = sys.argv[1:] or ["build"]
    if command == ["build"]:
        print(build())
    elif command == ["install"]:
        install()
    else:
        raise SystemExit("usage: makefile.py [build|install]")


if __name__ == "__main__":
    main()
