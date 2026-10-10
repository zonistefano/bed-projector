"""Checks the HA-to-firmware state contract without requiring Home Assistant."""

import importlib.util
from datetime import date, datetime, timedelta, timezone
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


ROME = timezone(timedelta(hours=2))


def hour(day, h, condition, temperature, rain=0):
    when = datetime(2026, 10, day, h, tzinfo=ROME).astimezone(timezone.utc)
    return {"datetime": when.isoformat(), "condition": condition, "temperature": temperature,
            "precipitation_probability": rain}


class ForecastTests(unittest.TestCase):
    def snapshot(self, **kwargs):
        options = {"weather": "weather.home"}
        states = {"weather.home": state("partlycloudy", temperature=17.34, apparent_temperature=17.6,
                                        humidity=71, wind_speed=1.84, wind_speed_unit="km/h")}
        return protocol.make_snapshot(states, options, today=date(2026, 10, 10), tz=ROME, **kwargs)

    def test_days_carry_dates_range_and_sun(self):
        daily = [{"datetime": "2026-10-09T22:00:00+00:00", "condition": "partlycloudy",
                  "temperature": 22.3, "templow": 13.5, "precipitation_probability": 50},
                 {"datetime": "2026-10-10T22:00:00+00:00", "condition": "rainy",
                  "temperature": 23, "templow": 15.3}]
        sun = {date(2026, 10, 10): (datetime(2026, 10, 10, 5, 33, tzinfo=timezone.utc),
                                    datetime(2026, 10, 10, 16, 47, tzinfo=timezone.utc))}
        days = self.snapshot(forecast=daily, sun=sun)["days"]
        self.assertEqual([d["date"] for d in days], ["2026-10-10", "2026-10-11"])
        self.assertEqual((days[0]["high"], days[0]["low"], days[0]["precipitation"]), (22, 14, 50))
        self.assertEqual((days[0]["sunrise"], days[0]["sunset"]), ("07:33", "18:47"))
        self.assertEqual((days[1]["condition"], days[1]["sunrise"]), ("rainy", ""))
        # Without hourly data the daytime falls back to the daily forecast.
        self.assertEqual(days[1]["day"], {"condition": "rainy", "temperature": 23, "precipitation": None})
        self.assertIsNone(days[1]["evening"])

    def test_hourly_daytime_and_evening(self):
        hourly = [hour(11, h, "sunny", 18 + h % 3) for h in range(8, 18)]
        hourly += [hour(11, 18, "rainy", 16, 70), hour(11, 19, "rainy", 15, 40),
                   hour(11, 20, "cloudy", 14), hour(11, 21, "cloudy", 13)]
        days = self.snapshot(hourly=hourly)["days"]
        self.assertEqual(days[1]["day"], {"condition": "sunny", "temperature": 20, "precipitation": 0})
        # Equal hours of rain and clouds: the more severe condition wins.
        self.assertEqual(days[1]["evening"], {"condition": "rainy", "temperature": 15, "precipitation": 70})
        self.assertIsNone(days[0]["day"])

    def test_twice_daily_night_is_the_evening_fallback(self):
        twice = [{"datetime": "2026-10-10T18:00:00+00:00", "is_daytime": False,
                  "condition": "clear-night", "temperature": 12, "precipitation_probability": 5}]
        days = self.snapshot(twice_daily=twice)["days"]
        self.assertEqual(days[0]["evening"], {"condition": "clear-night", "temperature": 12,
                                              "precipitation": 5})

    def test_current_details_and_summaries(self):
        options = {"weather": "weather.home", "indoor": "sensor.bedroom", "summary_today": "sensor.s0"}
        states = {"weather.home": state("sunny", temperature=17.34, apparent_temperature=17.6,
                                        humidity=71, wind_speed=1.84, wind_speed_unit="km/h"),
                  "sensor.bedroom": state("21.46"), "sensor.s0": state("Nubi sparse fino a sera.")}
        result = protocol.make_snapshot(states, options, today=date(2026, 10, 10), tz=ROME,
                                        light_names=["Sala", "Cucina"], opening_names=["É" * 40] + ["x"] * 9)
        self.assertEqual((result["indoor"], result["outdoor"]), ("21.5°", "17.3°"))
        self.assertEqual((result["temp_now"], result["feels"], result["humidity"], result["wind"]),
                         (17, "18°", 71, "2 km/h"))
        self.assertEqual((result["lights"], result["lights_known"]), (2, True))
        self.assertEqual(result["days"][0]["summary"], "Nubi sparse fino a sera.")
        self.assertEqual(len(result["opening_names"]), 8)
        self.assertLessEqual(len(result["opening_names"][0].encode()), 31)

    def test_lights_are_counted_from_names(self):
        # Names exclude the projector's own light, so the count must come from them,
        # never from a generic "lights on" helper.
        names = [f"Luce {i}" for i in range(10)]
        result = protocol.make_snapshot({"sensor.number_lights_on": state("11")},
                                        {"lights": "sensor.number_lights_on"}, light_names=names)
        self.assertEqual((result["lights"], result["lights_known"]), (10, True))
        self.assertEqual(len(result["light_names"]), 8)
        self.assertFalse(protocol.make_snapshot({}, {})["lights_known"])
        self.assertEqual(protocol.make_snapshot({}, {})["days"], [])

if __name__ == "__main__":
    unittest.main()
