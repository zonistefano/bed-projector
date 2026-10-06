"""Checks the HA-to-firmware state contract without requiring Home Assistant."""

import importlib.util
from pathlib import Path
from types import SimpleNamespace
import unittest


SOURCE = Path(__file__).resolve().parents[1] / "custom_components/bed_projector/protocol.py"
SPEC = importlib.util.spec_from_file_location("bed_projector_protocol", SOURCE)
protocol = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(protocol)


def state(value, **attributes):
    return SimpleNamespace(state=value, attributes=attributes)


class SnapshotTests(unittest.TestCase):
    def test_openings_and_distinct_alarm_states(self):
        options = {
            "openings": "sensor.number_open_contacts",
            "alarm": "alarm_control_panel.home",
            "weather": "weather.home",
            "entity1": "sensor.bedroom",
        }
        states = {
            "sensor.number_open_contacts": state("1"),
            "alarm_control_panel.home": state("armed_home"),
            "weather.home": state("sunny", temperature=19, temperature_unit="°C"),
            "sensor.bedroom": state("21", friendly_name="Bedroom", unit_of_measurement="°C"),
        }
        result = protocol.make_snapshot(states, options)
        self.assertEqual(result["openings"], 1)
        self.assertTrue(result["openings_known"])
        self.assertEqual(result["alarm"], "armed_home")
        self.assertEqual(result["temperature"], "19°C")
        self.assertEqual(result["extras"][0], "Bedroom: 21°C")
        for mode in ("armed_night", "armed_away", "armed_vacation", "pending", "triggered"):
            states["alarm_control_panel.home"] = state(mode)
            self.assertEqual(protocol.make_snapshot(states, options)["alarm"], mode)

    def test_open_contacts_count(self):
        options = {"openings": "sensor.number_open_contacts"}
        for raw, expected in (("0", 0), ("3.0", 3), ("250", 99)):
            result = protocol.make_snapshot({"sensor.number_open_contacts": state(raw)}, options)
            self.assertTrue(result["openings_known"])
            self.assertEqual(result["openings"], expected)

    def test_unusable_count_never_looks_closed(self):
        options = {"openings": "sensor.number_open_contacts"}
        for raw in ("unavailable", "unknown", "-1", "1.5", "nan"):
            result = protocol.make_snapshot({"sensor.number_open_contacts": state(raw)}, options)
            self.assertFalse(result["openings_known"], raw)
        self.assertFalse(protocol.make_snapshot({}, options)["openings_known"])
        self.assertEqual(protocol.make_snapshot({}, options)["alarm"], "unknown")

    def test_legacy_binary_sensor_list_is_unknown(self):
        options = {"openings": ["binary_sensor.door"]}
        result = protocol.make_snapshot({"binary_sensor.door": state("off")}, options)
        self.assertFalse(result["openings_known"])

    def test_today_forecast_condition_and_range(self):
        options = {"weather": "weather.home"}
        states = {"weather.home": state("cloudy", temperature=18.4)}
        forecast = [{"condition": "rainy", "temperature": 21.5, "templow": -3.5},
                    {"condition": "sunny", "temperature": 25, "templow": 12}]
        result = protocol.make_snapshot(states, options, forecast)
        self.assertEqual((result["condition"], result["temp_high"], result["temp_low"]), ("rainy", 22, -4))

    def test_forecast_falls_back_to_current_condition(self):
        options = {"weather": "weather.home"}
        result = protocol.make_snapshot({"weather.home": state("sunny")}, options)
        self.assertEqual((result["condition"], result["temp_high"], result["temp_low"]), ("sunny", None, None))
        result = protocol.make_snapshot({"weather.home": state("unavailable")}, options,
                                        [{"condition": "storm", "temperature": "n/a"}])
        self.assertEqual((result["condition"], result["temp_high"]), ("", None))

    def test_strings_fit_firmware_byte_buffers(self):
        options = {"entity1": "sensor.long", "weather": "weather.home"}
        states = {
            "sensor.long": state("123", friendly_name="É" * 30),
            "weather.home": state("☀" * 30),
        }
        result = protocol.make_snapshot(states, options)
        self.assertLessEqual(len(result["extras"][0].encode()), 31)
        self.assertLessEqual(len(result["weather"].encode()), 31)
        self.assertEqual(result["extras"][0].encode().decode(), result["extras"][0])


if __name__ == "__main__":
    unittest.main()
