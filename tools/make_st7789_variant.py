#!/usr/bin/env python3
"""Generate an ST7789 CYD build of ASCII Aquarium v2.39.

This keeps the upstream sketch easy to sync while applying only the board-specific
changes required by the TPM408-2.8 / ST7789 CYD variant.
"""
from pathlib import Path
import shutil

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "ASCII_Aquarium_CYD" / "ASCII_Aquarium_CYD_2.39.ino"
OUT_DIR = ROOT / "ASCII_Aquarium_CYD_ST7789"
OUT = OUT_DIR / "ASCII_Aquarium_CYD_ST7789.ino"
SETUP = ROOT / "User_Setup_ST7789_CYD.h"
SETUP_COPY = OUT_DIR / "User_Setup_ST7789_CYD.h"

text = SOURCE.read_text(encoding="utf-8")

replacements = {
    'static constexpr const char* kBoardProfileName = "CYD 2.8";':
        'static constexpr const char* kBoardProfileName = "CYD 2.8 ST7789";',
    'return displayFlip180 ? 3 : 1;':
        'return displayFlip180 ? 1 : 3;',
}

for old, new in replacements.items():
    count = text.count(old)
    if count != 1:
        raise SystemExit(f"Expected exactly one occurrence of {old!r}; found {count}. Upstream may have changed.")
    text = text.replace(old, new, 1)

text = text.replace(
    "// ASCII_Aquarium_CYD.ino | Version: v2.39",
    "// ASCII_Aquarium_CYD_ST7789.ino | Version: v2.39-ST7789",
    1,
)

OUT_DIR.mkdir(exist_ok=True)
OUT.write_text(text, encoding="utf-8")
shutil.copyfile(SETUP, SETUP_COPY)

print(f"Generated {OUT.relative_to(ROOT)}")
print(f"Copied    {SETUP_COPY.relative_to(ROOT)}")
print("ST7789 landscape rotation: 3; flipped rotation: 1")
