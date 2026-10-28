import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path


class ConversationLifecycleTest(unittest.TestCase):
    def test_stale_end_idempotency_and_late_speech_across_rewake(self):
        root = Path(__file__).resolve().parents[2]
        compiler = shutil.which('c++')
        self.assertIsNotNone(compiler)
        with tempfile.TemporaryDirectory() as directory:
            binary = str(Path(directory) / 'conversation_lifecycle_test')
            subprocess.run([compiler, '-std=c++17', '-Wall', '-Wextra', '-Werror',
                            '-I', str(root / 'main'),
                            str(root / 'scripts/tests/conversation_lifecycle_test.cc'), '-o', binary],
                           check=True, timeout=60)
            subprocess.run([binary], check=True, timeout=10)
