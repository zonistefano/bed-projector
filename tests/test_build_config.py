"""Build-time .env parsing and validation, without using the local secret file."""

import importlib.util
from pathlib import Path
import re
import unittest


SOURCE = Path(__file__).resolve().parents[1] / "tools/generate_build_config.py"
SPEC = importlib.util.spec_from_file_location("build_config_generator", SOURCE)
generator = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(generator)


class BuildConfigTests(unittest.TestCase):
    def test_literal_values_and_legacy_keys(self):
        values = generator.parse_env(
            'ARTLOGIC_SSID=ignored\nWIFI_SSID="Casa 2G"\n'
            'WIFI_PASSWORD="A$pass#word"\nAPI_TOKEN=' + 'a' * 32 + '\n'
        )
        self.assertEqual(values["WIFI_SSID"], "Casa 2G")
        self.assertEqual(values["WIFI_PASSWORD"], "A$pass#word")
        output = generator.render(values)
        self.assertNotIn("A$pass#word", output)
        self.assertIn("BED_BUILD_WIFI_FINGERPRINT", output)

    def test_wifi_needs_valid_token(self):
        with self.assertRaisesRegex(ValueError, "API_TOKEN"):
            generator.render({"WIFI_SSID": "Casa", "WIFI_PASSWORD": "abcdefgh"})
        with self.assertRaisesRegex(ValueError, "API_TOKEN"):
            generator.render({"WIFI_SSID": "Casa", "API_TOKEN": "password"})

    def test_wifi_edit_changes_fingerprint(self):
        first = generator.render({"WIFI_SSID": "Casa", "WIFI_PASSWORD": "abcdefgh", "API_TOKEN": "a" * 32})
        second = generator.render({"WIFI_SSID": "Casa", "WIFI_PASSWORD": "abcdefghX", "API_TOKEN": "a" * 32})
        pattern = r'BED_BUILD_WIFI_FINGERPRINT "([0-9a-f]+)"'
        self.assertNotEqual(re.search(pattern, first).group(1), re.search(pattern, second).group(1))

    def test_rejects_duplicate_and_control_character(self):
        with self.assertRaisesRegex(ValueError, "Duplicate WIFI_SSID"):
            generator.parse_env("WIFI_SSID=Casa\nWIFI_SSID=Altro\n")
        with self.assertRaisesRegex(ValueError, "control character"):
            generator.render({"WIFI_SSID": "bad\tssid"})


if __name__ == "__main__":
    unittest.main()
