#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 Duet Plus contributors
# SPDX-License-Identifier: GPL-3.0-or-later
"""Private Switch 1 Android bring-ups must not depend on the published README."""
import importlib.util
from pathlib import Path
import tempfile

spec = importlib.util.spec_from_file_location("native_support_catalog", Path(__file__).with_name("native-support-catalog.py"))
catalog = importlib.util.module_from_spec(spec)
spec.loader.exec_module(catalog)

with tempfile.TemporaryDirectory() as directory:
    root = Path(directory)
    (root / "README.md").write_text("", encoding="utf-8")
    android = root / "android/app"
    android.mkdir(parents=True)
    (android / "gradle.properties").write_text(
        "nxGames=0400486024DEC000,01007A2027548000\n", encoding="utf-8")
    assert catalog.supported(root, "peanuts", {}) == catalog.PRIVATE_PEANUTS | {"01007A2027548000"}
print("Private Android roster recognized; Switch 2 entry excluded from Peanuts routing.")
