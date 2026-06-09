#!/usr/bin/env python3
"""Generate scroll_capture.rc with VERSIONINFO from VERSION file."""

from __future__ import annotations

from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
VERSION_FILE = ROOT.parent / "VERSION"
RC_FILE = ROOT / "scroll_capture.rc"
MANIFEST = ROOT / "scroll_capture.manifest"


def parse_version(text: str) -> tuple[int, int, int, int]:
    parts = text.strip().split(".")
    if len(parts) != 3:
        raise SystemExit(f"Invalid VERSION: {text!r}")
    major, minor, patch = (int(p) for p in parts)
    return major, minor, patch, 0


def main() -> int:
    version_text = VERSION_FILE.read_text(encoding="utf-8").strip()
    major, minor, patch, build = parse_version(version_text)
    comma = f"{major},{minor},{patch},{build}"
    dotted = f"{major}.{minor}.{patch}.{build}"

    rc = f"""// Auto-generated from VERSION — do not edit by hand.

#include <winver.h>

1 RT_MANIFEST "{MANIFEST.name}"

VS_VERSION_INFO VERSIONINFO
FILEVERSION {comma}
PRODUCTVERSION {comma}
FILEFLAGSMASK VS_FFI_FILEFLAGSMASK
FILEFLAGS 0
FILEOS VOS_NT_WINDOWS32
FILETYPE VFT_APP
FILESUBTYPE VFT2_UNKNOWN
BEGIN
    BLOCK "StringFileInfo"
    BEGIN
        BLOCK "040904b0"
        BEGIN
            VALUE "CompanyName", "Scroll Capture"
            VALUE "FileDescription", "Long screenshot capture for VRM/RDP"
            VALUE "FileVersion", "{dotted}"
            VALUE "InternalName", "scroll_capture"
            VALUE "LegalCopyright", "Open source"
            VALUE "OriginalFilename", "scroll_capture.exe"
            VALUE "ProductName", "Scroll Capture"
            VALUE "ProductVersion", "{dotted}"
        END
    END
    BLOCK "VarFileInfo"
    BEGIN
        VALUE "Translation", 0x409, 1200
    END
END
"""
    RC_FILE.write_text(rc, encoding="utf-8")
    print(f"Generated {RC_FILE} for version {dotted}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
