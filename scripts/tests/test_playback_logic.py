import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]


class PlaybackLogicTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which("c++")
        if not compiler:
            raise RuntimeError("A host C++ compiler is required for playback logic tests")
        cls.directory = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.directory.cleanup)
        cls.binary = Path(cls.directory.name) / "playback_logic_test"
        subprocess.run(
            [compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pthread",
             "-I", str(ROOT / "main/boards/ivy-robot-v2"),
             str(ROOT / "scripts/tests/playback_logic_test.cc"), "-o", str(cls.binary)],
            check=True, timeout=60,
        )

    def test_eof_and_metadata(self):
        subprocess.run([str(self.binary), "eof"], check=True, timeout=10)

    def test_cancelled_worker_and_copied_result(self):
        subprocess.run([str(self.binary), "cancellation"], check=True, timeout=10)

    def test_stop_deduplication_and_owner(self):
        subprocess.run([str(self.binary), "stop_owner"], check=True, timeout=10)

    def test_remaining_body_read_budget(self):
        subprocess.run([str(self.binary), "read_budget"], check=True, timeout=10)

    def test_headerless_ape_through_frame_scanner(self):
        subprocess.run([str(self.binary), "ape_pipeline"], check=True, timeout=10)

    def test_downloaded_audio_artifact(self):
        artifact = os.environ.get("IVY_STUDY_AUDIO_ARTIFACT")
        if not artifact:
            self.skipTest("Set IVY_STUDY_AUDIO_ARTIFACT to a downloaded MP3; media is not stored in git")
        for chunk in (1, 64, 208, 209, 512, 1500, 4096):
            with self.subTest(chunk=chunk):
                result = subprocess.run(
                    [str(self.binary), "artifact", artifact, str(chunk)],
                    check=True, timeout=30, capture_output=True, text=True,
                )
                self.assertIn("completed=1", result.stdout)
                self.assertIn("overflow=0", result.stdout)
