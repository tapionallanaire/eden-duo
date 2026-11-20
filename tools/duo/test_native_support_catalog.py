#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 Duet Plus contributors
# SPDX-License-Identifier: GPL-3.0-or-later
"""Only published, declared or reviewed Android profiles belong to Duet."""
import importlib.util
from pathlib import Path
import tempfile
from types import SimpleNamespace

spec = importlib.util.spec_from_file_location("native_support_catalog", Path(__file__).with_name("native-support-catalog.py"))
catalog = importlib.util.module_from_spec(spec)
spec.loader.exec_module(catalog)

with tempfile.TemporaryDirectory() as directory:
    root = Path(directory)
    (root / "peanuts").mkdir()
    (root / "peanuts/release-titles.txt").write_text("0100000000010000\n", encoding="utf-8")
    android = root / "android/app"
    android.mkdir(parents=True)
    (android / "gradle.properties").write_text(
        "nxGames=0400486024DEC000,01007A2027548000\n", encoding="utf-8")
    games = {}
    for title, readiness in (("010090C022978003", "status\tbring-up\tprogram 3"),
                             ("01003FC01670C000", "required\tfeature.shaderInt64\t1\tGPU feature"),
                             ("0400486024DEC000", "status\tplayable")):
        directory = root / "dm/games" / title
        directory.mkdir(parents=True)
        (directory / "readiness.tsv").write_text(readiness + "\n", encoding="utf-8")
        games[title] = SimpleNamespace(dir=directory)
    included = catalog.supported(root, "peanuts", games)
    assert {"0100000000010000", "010090C022978003", "01007A2027548000",
            "010057D006492000", "0100F7E00C70E000", "0100F43008C44000"} <= included
    assert not {"01003FC01670C000", "0400486024DEC000", "010090C022978000"} & included
print("Release, declared and reviewed Android profiles recognized; unrelated profiles excluded.")
