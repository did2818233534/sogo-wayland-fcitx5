import importlib.util
import json
from pathlib import Path
import tempfile
import unittest

REPO = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('deploy', REPO / 'scripts/deploy.py')
deploy = importlib.util.module_from_spec(spec)
spec.loader.exec_module(deploy)


class DeploymentTests(unittest.TestCase):
    def test_archived_packages_match_checksums(self):
        self.assertEqual(len(deploy.verify_packages()), 7)

    def test_corrupt_package_rejected(self):
        with tempfile.TemporaryDirectory() as d:
            root = Path(d)
            (root / 'vendor/debs').mkdir(parents=True)
            (root / 'vendor/debs/a.deb').write_bytes(b'bad')
            (root / 'vendor/manifest.json').write_text(json.dumps([
                {'file': 'a.deb', 'sha256': '0' * 64}]))
            with self.assertRaisesRegex(ValueError, 'Checksum mismatch'):
                deploy.verify_packages(root)

    def test_existing_file_and_dangling_link_both_conflict(self):
        with tempfile.TemporaryDirectory() as d:
            root = Path(d)
            f = root / 'file'
            f.write_text('preserve')
            link = root / 'link'
            link.symlink_to(root / 'missing')
            self.assertEqual(deploy.conflicts({str(f): '', str(link): ''}), [str(f), str(link)])
            self.assertEqual(f.read_text(), 'preserve')
            self.assertTrue(link.is_symlink())

    def test_link_plan_points_into_install_prefix(self):
        with tempfile.TemporaryDirectory() as d:
            root = Path(d)
            lib = root / 'usr/lib/x86_64-linux-gnu'
            lib.mkdir(parents=True)
            (lib / 'libcpis-engine.so').touch()
            links = deploy.link_plan(root)
            self.assertEqual(links['/etc/cpis'], '/opt/sogou-ng/root/etc/cpis')
            self.assertEqual(links['/usr/lib/x86_64-linux-gnu/libcpis-engine.so'],
                             '/opt/sogou-ng/root/usr/lib/x86_64-linux-gnu/libcpis-engine.so')
