"""Build-context manifest tests."""
import hashlib
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest

SPEC = importlib.util.spec_from_file_location(
    'build_context', Path(__file__).parent / 'tools/build_context.py')
build_context = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(build_context)


class BuildContextTest(unittest.TestCase):
    def test_collect_records_product_panel_and_artifact_hash(self):
        with tempfile.TemporaryDirectory() as tmp:
            build = Path(tmp) / 'build'
            build.mkdir()
            (build / 'mt_product_profile.h').write_text(
                '#define MT_PRODUCT_NAME "s3_reference"\n'
                '#define MT_PRODUCT_DEVICE_PLATFORM "ESP32-P4"\n'
                '#define MT_PRODUCT_DEVICE_PANEL "368 x 448 AMOLED"\n',
                encoding='utf-8',
            )
            config = build / 'gen_inc/sdkconfig.h'
            config.parent.mkdir()
            config.write_text(
                '#define CONFIG_IDF_TARGET "esp32p4"\n',
                encoding='utf-8',
            )
            artifact = build / 'microtech_sim'
            artifact.write_bytes(b'fake-binary')
            context = build_context.collect(build, artifact, 'sim')
            self.assertEqual(context['schema'], 1)
            self.assertEqual(context['kind'], 'sim')
            self.assertEqual(context['target'], 'esp32p4')
            self.assertEqual(context['product'], 's3_reference')
            self.assertEqual(context['panel'], {'width': 368, 'height': 448})
            self.assertEqual(
                context['artifact_sha256'],
                hashlib.sha256(b'fake-binary').hexdigest(),
            )
            self.assertEqual(
                context['config_sha256'],
                hashlib.sha256(config.read_bytes()).hexdigest(),
            )
            self.assertEqual(context['artifact_size'], len(b'fake-binary'))

    def test_collect_rejects_missing_artifact(self):
        with tempfile.TemporaryDirectory() as tmp:
            build = Path(tmp)
            with self.assertRaises(FileNotFoundError):
                build_context.collect(build, build / 'missing.bin', 'sim')


if __name__ == '__main__':
    unittest.main()
