# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

"""Muse Jam's sound engine (components/muse/muse_jam_synth.c): pads in tune,
a mix that doesn't crack, and a listener that finds a beat's tempo and lands
SYNC on it."""

from __future__ import annotations

import json
import os
import shlex
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
HOUSE, BOOM_BAP, TRAP = 0, 1, 2
PRESETS = ("PLUCK", "NEON", "GLASS", "CHIP")


class JamTest(unittest.TestCase):
    binary: Path

    @classmethod
    def setUpClass(cls) -> None:
        cc = shlex.split(os.environ.get("CC", "cc"))
        if not cc or shutil.which(cc[0]) is None:
            raise unittest.SkipTest("C compiler not available")
        cls.tmp = tempfile.TemporaryDirectory()
        cls.binary = Path(cls.tmp.name) / "muse_jam_harness"
        proc = subprocess.run(
            [
                *cc,
                "-std=gnu11",
                "-O2",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-I",
                str(ROOT / "components" / "muse"),
                str(ROOT / "tests" / "muse_jam_harness.c"),
                str(ROOT / "components" / "muse" / "muse_jam_synth.c"),
                "-lm",
                "-o",
                str(cls.binary),
            ],
            cwd=ROOT,
            text=True,
            capture_output=True,
        )
        if proc.returncode:
            raise AssertionError(proc.stdout + proc.stderr)

    @classmethod
    def tearDownClass(cls) -> None:
        cls.tmp.cleanup()

    def run_harness(self, *commands: str) -> list[dict]:
        proc = subprocess.run([str(self.binary)], input="\n".join(commands) + "\n", text=True,
                              capture_output=True, timeout=120)
        self.assertEqual(proc.returncode, 0, msg=proc.stderr)
        return [json.loads(line) for line in proc.stdout.splitlines()]

    def test_pads_are_in_tune(self) -> None:
        cases = [(p, pad) for p in range(len(PRESETS)) for pad in (0, 7, 14)]
        for (preset, pad), r in zip(cases, self.run_harness(*(f"pitch {p} {pad}" for p, pad in cases))):
            with self.subTest(preset=PRESETS[preset], pad=pad, hz=r["hz"]):
                self.assertGreater(r["at"], 20.0, "too quiet")
                self.assertGreater(r["at"], 5 * max(r["below"], r["above"]), "not at its note")

    def test_mix_does_not_crack(self) -> None:
        r, = self.run_harness("mix")
        self.assertGreater(r["rms_db"], -24.0)
        self.assertLess(r["rms_db"], -6.0)
        self.assertLess(r["clipped"], 0.005)

    def test_hears_the_tempo(self) -> None:
        cases = [(p, bpm) for p in (HOUSE, BOOM_BAP, TRAP) for bpm in (90, 100, 120, 140)]
        for (p, bpm), r in zip(cases, self.run_harness(*(f"tempo {p} {bpm}" for p, bpm in cases))):
            with self.subTest(pattern=p, bpm=bpm):
                self.assertAlmostEqual(r["tempo"], bpm, delta=1.5)

    def test_sync_lands_on_the_beat(self) -> None:
        cases = [(HOUSE, 96), (HOUSE, 118), (HOUSE, 128), (BOOM_BAP, 90), (BOOM_BAP, 104)]
        for (p, bpm), r in zip(cases, self.run_harness(*(f"sync {p} {bpm}" for p, bpm in cases))):
            with self.subTest(pattern=p, bpm=bpm):
                self.assertAlmostEqual(r["tempo"], bpm, delta=1.5)
                self.assertEqual(r["beats"], 12)
                self.assertLess(r["worst_ms"], 20.0)


if __name__ == "__main__":
    unittest.main()
