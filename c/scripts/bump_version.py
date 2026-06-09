#!/usr/bin/env python3
"""Bump patch version in VERSION, scroll_capture.py, and c/include/version.h."""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
VERSION_FILE = ROOT / "VERSION"
VERSION_H = ROOT / "c" / "include" / "version.h"
PY_FILE = ROOT / "scroll_capture.py"


def read_version() -> str:
    return VERSION_FILE.read_text(encoding="utf-8").strip()


def bump_patch(version: str) -> str:
    parts = version.split(".")
    if len(parts) != 3:
        raise SystemExit(f"Invalid VERSION format: {version!r} (expected MAJOR.MINOR.PATCH)")
    major, minor, patch = parts
    return f"{major}.{minor}.{int(patch) + 1}"


def write_version_h(version: str) -> None:
    VERSION_H.write_text(
        "#ifndef SC_VERSION_H\n"
        "#define SC_VERSION_H\n"
        f'#define SC_VERSION "{version}"\n'
        "#endif\n",
        encoding="utf-8",
    )


def update_python(version: str) -> None:
    text = PY_FILE.read_text(encoding="utf-8")
    if "__version__" in text:
        text = re.sub(r'__version__\s*=\s*"[^"]+"', f'__version__ = "{version}"', text, count=1)
    else:
        text = text.replace(
            '"""',
            f'"""\n\n__version__ = "{version}"',
            1,
        )
    PY_FILE.write_text(text, encoding="utf-8")


def main() -> int:
    current = read_version()
    new_version = bump_patch(current)
    VERSION_FILE.write_text(new_version + "\n", encoding="utf-8")
    write_version_h(new_version)
    update_python(new_version)
    print(new_version)
    return 0


if __name__ == "__main__":
    sys.exit(main())
