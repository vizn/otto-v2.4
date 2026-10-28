import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path


class ResourceUrlTest(unittest.TestCase):
    def test_resources_follow_successful_ota_origin(self):
        root = Path(__file__).resolve().parents[2]
        with tempfile.TemporaryDirectory() as directory:
            binary = str(Path(directory) / 'resource_url_test')
            subprocess.run([shutil.which('c++'), '-std=c++17', '-Wall', '-Wextra', '-Werror',
                            '-I', str(root / 'main'), str(root / 'scripts/tests/resource_url_test.cc'),
                            '-o', binary], check=True, timeout=30)
            subprocess.run([binary], check=True, timeout=10)
