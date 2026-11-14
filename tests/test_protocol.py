"""Checks the HA-to-firmware state contract without requiring Home Assistant."""

import importlib
from datetime import date, datetime, timedelta, timezone
from pathlib import Path
import sys
from types import ModuleType, SimpleNamespace
import unittest


# Load the integration's modules without running its __init__, which needs Home Assistant.
PACKAGE = ModuleType("bed_projector")
PACKAGE.__path__ = [str(Path(__file__).resolve().parents[1] / "custom_components/bed_projector")]
sys.modules.setdefault("bed_projector", PACKAGE)
protocol = importlib.import_module("bed_projector.protocol")


def state(value, **attributes):
    return SimpleNamespace(state=value, attributes=attributes)


class SnapshotTests(unittest.TestCase):
    def test_openings_and_distinct_alarm_states(self):
        options = {
            "alarm": "alarm_control_panel.home",
            "weather": "weather.home",
            "entity1": "sensor.bedroom",
        }
        states = {
            "alarm_control_panel.home": state("armed_home"),
            "weather.home": state("sunny", temperature=19, temperature_unit="°C"),
            "sensor.bedroom": state("21", friendly_name="Bedroom", unit_of_measurement="°C"),
        }
        result = protocol.make_snapshot(states, options, opening_names=["Finestra sala"])
        self.assertEqual(result["openings"], 1)
        self.assertTrue(result["openings_known"])
        self.assertEqual(result["alarm"], "armed_home")
        self.assertEqual(result["temperature"], "19°C")
        self.assertEqual(result["extras"][0], "Bedroom: 21°C")
        for mode in ("armed_night", "armed_away", "armed_vacation", "pending", "triggered"):
            states["alarm_control_panel.home"] = state(mode)
            self.assertEqual(protocol.make_snapshot(states, options)["alarm"], mode)

    def test_openings_are_counted_from_names(self):
        result = protocol.make_snapshot({}, {}, opening_names=[f"Finestra {i}" for i in range(120)])
        self.assertEqual((result["openings"], result["openings_known"]), (99, True))
        self.assertEqual(len(result["opening_names"]), 8)
        result = protocol.make_snapshot({}, {}, opening_names=[])
        self.assertEqual((result["openings"], result["openings_known"]), (0, True))

    def test_unknown_names_never_look_closed(self):
        # No names (e.g. a failed template) is unknown, not "all closed".
        result = protocol.make_snapshot({}, {})
        self.assertFalse(result["openings_known"])
        self.assertEqual(result["alarm"], "unknown")

    def test_unavailable_entities_are_unknown(self):
        self.assertFalse(protocol.available(None))
        self.assertFalse(protocol.available(state("unavailable")))
        self.assertTrue(protocol.available(state("0")))
        options = {"weather": "weather.home", "indoor": "sensor.bedroom", "entity1": "sensor.x"}
        states = {"weather.home": state("unavailable", temperature=20),
                  "sensor.bedroom": state("unknown"), "sensor.x": state("unavailable")}
        result = protocol.make_snapshot(states, options)
        self.assertEqual((result["weather"], result["temperature"], result["outdoor"], result["indoor"]),
                         ("", "", "", ""))
        self.assertIsNone(result["temp_now"])
        self.assertEqual(result["extras"], ["", "", "", ""])

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
        days = self.snapshot(daily=daily, sun=sun)["days"]
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

    def test_template_names(self):
        self.assertEqual(protocol.template_names("Cucina, Sala ,, Bagno"), ["Cucina", "Sala", "Bagno"])
        self.assertEqual(protocol.template_names("Cucina\nSala"), ["Cucina", "Sala"])
        self.assertEqual(protocol.template_names(["Cucina", 3, " "]), ["Cucina", "3"])
        self.assertEqual(protocol.template_names("  "), [])
        # A failed or unavailable template is unknown, never "all off".
        self.assertIsNone(protocol.template_names(None))
        self.assertIsNone(protocol.template_names("unavailable"))
        result = protocol.make_snapshot({}, {}, light_names=protocol.template_names("A, B, C"))
        self.assertEqual((result["lights"], result["light_names"]), (3, ["A", "B", "C"]))
        self.assertFalse(protocol.make_snapshot({}, {}, light_names=protocol.template_names(None))["lights_known"])

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


NOW = datetime(2026, 10, 10, 14, 35, tzinfo=ROME)


def event(start, end, summary):
    return {"start": start, "end": end, "summary": summary}


class AgendaTests(unittest.TestCase):
    def test_wake_time(self):
        alarm = state("2026-10-11T04:45:00+00:00", device_class="timestamp")
        self.assertEqual(protocol.wake_time(alarm, NOW), (True, {"date": "2026-10-11", "time": "06:45"}))
        # Past alarms and alarms beyond a week are none; unusable sensors are unknown.
        self.assertEqual(protocol.wake_time(state("2026-10-10T10:00:00+00:00"), NOW), (True, None))
        self.assertEqual(protocol.wake_time(state("2026-10-18T10:00:00+00:00"), NOW), (True, None))
        self.assertEqual(protocol.wake_time(state("unavailable"), NOW), (False, None))
        self.assertEqual(protocol.wake_time(state("2026-10-11T06:45:00"), NOW), (False, None))
        self.assertEqual(protocol.wake_time(state("domani"), NOW), (False, None))
        self.assertEqual(protocol.wake_time(None, NOW), (False, None))

    def test_wake_time_input_datetime(self):
        daily = {"has_date": False, "has_time": True}
        # A time-only helper rings at its next occurrence, in HA's local time.
        self.assertEqual(protocol.wake_time(state("06:45:00", **daily), NOW),
                         (True, {"date": "2026-10-11", "time": "06:45"}))
        self.assertEqual(protocol.wake_time(state("18:00:00", **daily), NOW),
                         (True, {"date": "2026-10-10", "time": "18:00"}))
        # It stays today's alarm for the whole minute it rings.
        self.assertEqual(protocol.wake_time(state("14:35:00", **daily), NOW.replace(second=40)),
                         (True, {"date": "2026-10-10", "time": "14:35"}))
        dated = {"has_date": True, "has_time": True}
        self.assertEqual(protocol.wake_time(state("2026-10-12 07:30:00", **dated), NOW),
                         (True, {"date": "2026-10-12", "time": "07:30"}))
        self.assertEqual(protocol.wake_time(state("2026-10-09 07:30:00", **dated), NOW), (True, None))
        # A date-only helper is not an alarm; garbage is unknown.
        self.assertEqual(protocol.wake_time(state("2026-10-11", has_date=True, has_time=False), NOW),
                         (False, None))
        self.assertEqual(protocol.wake_time(state("presto", **daily), NOW), (False, None))

    def test_wake_time_enabled(self):
        # The helper always holds a time: the on/off entity decides whether it rings.
        alarm = state("06:45:00", has_date=False, has_time=True)
        self.assertEqual(protocol.wake_time(alarm, NOW, state("on")),
                         (True, {"date": "2026-10-11", "time": "06:45"}))
        self.assertEqual(protocol.wake_time(alarm, NOW, state("off")), (True, None))
        self.assertEqual(protocol.wake_time(alarm, NOW, state("unavailable")), (False, None))
        self.assertEqual(protocol.wake_time(state("unavailable"), NOW, state("on")), (False, None))

    def test_snapshot_wake(self):
        options = {"next_alarm": "sensor.phone_next_alarm"}
        states = {"sensor.phone_next_alarm": state("2026-10-11T04:45:00+00:00")}
        result = protocol.make_snapshot(states, options, now=NOW)
        self.assertEqual((result["wake_known"], result["wake"]),
                         (True, {"date": "2026-10-11", "time": "06:45"}))
        self.assertTrue(result["wake_enabled"])
        disabled = {**options, "next_alarm_enabled": "input_boolean.wake_enabled"}
        on = {**states, "input_boolean.wake_enabled": state("on")}
        self.assertTrue(protocol.make_snapshot(on, disabled, now=NOW)["wake_enabled"])
        past = {**on, "sensor.phone_next_alarm": state("2026-10-09T04:45:00+00:00")}
        result = protocol.make_snapshot(past, disabled, now=NOW)
        self.assertEqual((result["wake_known"], result["wake_enabled"], result["wake"]),
                         (True, True, None))
        off = {**states, "input_boolean.wake_enabled": state("off")}
        result = protocol.make_snapshot(off, disabled, now=NOW)
        self.assertEqual((result["wake_known"], result["wake"]), (True, None))
        self.assertFalse(result["wake_enabled"])
        unavailable = {**states, "input_boolean.wake_enabled": state("unavailable")}
        self.assertFalse(protocol.make_snapshot(unavailable, disabled, now=NOW)["wake_enabled"])
        self.assertEqual((result["events_known"], result["events"]), (False, []))
        result = protocol.make_snapshot({}, {}, now=NOW)
        self.assertEqual((result["wake_known"], result["wake"]), (False, None))
        self.assertFalse(result["wake_enabled"])
        self.assertFalse(protocol.make_snapshot(states, options)["wake_known"])

    def test_upcoming_events(self):
        items = [
            event("2026-10-11T07:00:00+02:00", "2026-10-11T08:00:00+02:00", "Corsa al parco"),
            event("2026-10-10T18:30:00+02:00", "2026-10-10T21:00:00+02:00", "Cena da #Marco"),
            event("2026-10-11", "2026-10-12", "Compleanno Giulia"),
            event("2026-10-10T08:00:00+02:00", "2026-10-10T09:00:00+02:00", "Già finito"),
            event("2026-10-12T09:00:00+02:00", "2026-10-12T10:00:00+02:00", "Dopodomani"),
            event("2026-10-11T09:00:00+02:00", "2026-10-11T10:00:00+02:00", ""),
        ]
        self.assertEqual(protocol.upcoming_events(items, NOW), [
            {"date": "2026-10-10", "time": "18:30", "title": "Cena da Marco"},
            {"date": "2026-10-11", "time": "", "title": "Compleanno Giulia"},
            {"date": "2026-10-11", "time": "07:00", "title": "Corsa al parco"},
        ])

    def test_ongoing_events_are_today(self):
        items = [
            event("2026-10-09", "2026-10-12", "Ferie"),
            event("2026-10-09T22:00:00+02:00", "2026-10-10T16:00:00+02:00", "Turno"),
            event("2026-10-10T14:00:00+02:00", "2026-10-10T15:00:00+02:00", "Riunione"),
            event("2026-10-10T16:00:00+00:00", "2026-10-10T17:00:00+00:00", "x" * 40),
        ]
        result = protocol.upcoming_events(items, NOW)
        self.assertEqual(result[:3], [
            {"date": "2026-10-10", "time": "", "title": "Ferie"},
            {"date": "2026-10-10", "time": "", "title": "Turno"},
            {"date": "2026-10-10", "time": "14:00", "title": "Riunione"},
        ])
        self.assertEqual(len(protocol.upcoming_events(items[3:], NOW)[0]["title"].encode()), 31)
        self.assertEqual(protocol.upcoming_events(items[3:], NOW)[0]["time"], "18:00")

    def test_snapshot_events(self):
        items = [event("2026-10-10T18:30:00+02:00", "2026-10-10T21:00:00+02:00", "Cena")]
        result = protocol.make_snapshot({}, {}, now=NOW, events=items)
        self.assertEqual((result["events_known"], len(result["events"])), (True, 1))
        result = protocol.make_snapshot({}, {}, now=NOW, events=[])
        self.assertEqual((result["events_known"], result["events"]), (True, []))


class ThermostatTests(unittest.TestCase):
    OPTIONS = {"climate_heat": "climate.radiators", "climate_cool": "climate.ac", "indoor": "sensor.bedroom"}

    def test_matches_firmware(self):
        import re
        root = Path(__file__).resolve().parents[1]
        source = (root / "main/projector.c").read_text()
        for name, values in (("hvac_modes", protocol.HVAC_MODES), ("hvac_actions", protocol.HVAC_ACTIONS)):
            block = re.search(name + r"\[\] = \{(.*?)\};", source, re.S).group(1)
            self.assertEqual(tuple(re.findall(r'"([^"]+)"', block)), values)
        header = (root / "main/projector.h").read_text()
        limits = tuple(int(re.search(rf"#define PROJECTOR_TENTHS_{bound} (-?\d+)", header).group(1))
                       for bound in ("MIN", "MAX"))
        self.assertEqual(limits, protocol.TENTHS_RANGE)

    def test_source(self):
        source = protocol.thermostat_source
        heat_on = {"climate.radiators": state("heat", hvac_action="heating"), "climate.ac": state("off")}
        self.assertIsNone(source({}, {}, "auto", 1))
        self.assertEqual(source({}, {"climate_cool": "climate.ac"}, "winter", 1), "cool")
        self.assertEqual(source(heat_on, self.OPTIONS, "summer", 1), "cool")
        self.assertEqual(source({}, self.OPTIONS, "winter", 7), "heat")
        self.assertEqual(source(heat_on, self.OPTIONS, "auto", 7), "heat")
        both_on = {"climate.radiators": state("heat", hvac_action="idle"),
                   "climate.ac": state("cool", hvac_action="cooling")}
        self.assertEqual(source(both_on, self.OPTIONS, "auto", 1), "cool")
        both_off = {"climate.radiators": state("off"), "climate.ac": state("unavailable")}
        self.assertEqual(source(both_off, self.OPTIONS, "auto", 11), "heat")
        self.assertEqual(source(both_off, self.OPTIONS, "auto", 7), "cool")

    def test_payload(self):
        radiators = state("heat", hvac_action="heating", temperature=21.5, current_temperature=19.26,
                          min_temp=5, max_temp=30, current_humidity=47.6, preset_mode="eco")
        self.assertEqual(protocol.thermostat(radiators, "heat", None), {
            "source": "heat", "mode": "heat", "action": "heating", "current": 193, "target": 215,
            "min": 100, "max": 300, "humidity": 48, "preset": "Eco"})
        ac = state("heat_cool", target_temp_low=20, target_temp_high=26, min_temp=16, max_temp=32,
                   hvac_action="sleeping", preset_mode="none")
        result = protocol.thermostat(ac, "cool", state("27.44"))
        self.assertEqual((result["target"], result["current"], result["min"], result["max"]), (260, 274, 160, 300))
        self.assertEqual((result["action"], result["preset"], result["humidity"]), (None, "", None))
        hot = protocol.thermostat(state("heat", temperature=32, min_temp=31, max_temp=28), "heat", None)
        self.assertEqual((hot["min"], hot["max"]), (100, 320))
        off = protocol.thermostat(state("off", current_temperature="x"), "heat", state("unknown"))
        self.assertEqual((off["mode"], off["target"], off["current"]), ("off", None, None))
        self.assertIsNone(protocol.thermostat(state("unavailable"), "heat", None))
        self.assertIsNone(protocol.thermostat(state("boost"), "heat", None))
        self.assertIsNone(protocol.thermostat(None, "heat", None))
        self.assertEqual(protocol.tenth_degrees(-0.25), -3)
        self.assertIsNone(protocol.tenth_degrees(100.1))
        self.assertEqual(len(protocol.thermostat(state("heat", preset_mode="É" * 20), "heat", None)
                             ["preset"].encode()), 14)

    def test_snapshot(self):
        self.assertIsNone(protocol.make_snapshot({}, {})["thermostat"])
        states = {"climate.radiators": state("heat", temperature=21), "climate.ac": state("cool", temperature=25),
                  "sensor.bedroom": state("20.5")}
        result = protocol.make_snapshot(states, self.OPTIONS, today=date(2026, 7, 1), tz=ROME)
        self.assertEqual((result["thermostat"]["source"], result["thermostat"]["current"]), ("cool", 205))
        result = protocol.make_snapshot(states, self.OPTIONS, season="winter")
        self.assertEqual((result["thermostat"]["source"], result["thermostat"]["target"]), ("heat", 210))
        states["climate.radiators"] = state("unavailable")
        self.assertIsNone(protocol.make_snapshot(states, self.OPTIONS, season="winter")["thermostat"])


class MessageTests(unittest.TestCase):
    def test_icons_match_firmware(self):
        import re
        source = (Path(__file__).resolve().parents[1] / "main/projector.c").read_text()
        block = re.search(r"message_icons\[\] = \{(.*?)\};", source, re.S).group(1)
        const = importlib.import_module("bed_projector.const")
        self.assertEqual(tuple(re.findall(r'"([^"]+)"', block)), const.MESSAGE_ICONS)

    def test_payload(self):
        self.assertEqual(protocol.message_payload("Lavatrice finita", "washing-machine", 60),
                         {"text": "Lavatrice finita", "icon": "washing-machine", "duration": 60})
        self.assertEqual(protocol.message_payload(" Ciao "), {"text": "Ciao", "duration": 30})
        self.assertLessEqual(len(protocol.message_payload("è" * 50)["text"].encode()), 63)
        self.assertEqual(protocol.message_payload("#ff0000 Rosso#")["text"], "ff0000 Rosso")
        self.assertEqual(protocol.message_payload("Fumo", None, 0, "alert", True),
                         {"text": "Fumo", "duration": 0, "level": "alert", "blink": True})
        self.assertEqual(protocol.message_payload("Pacco", None, 60, "warning"),
                         {"text": "Pacco", "duration": 60, "level": "warning"})
        for args in (("",), ("  ",), ("#",), ("Ciao", "unicorn"), ("Ciao", None, 4), ("Ciao", None, 3601),
                     ("Ciao", None, 30, "panic")):
            with self.assertRaises(ValueError):
                protocol.message_payload(*args)

    def test_levels_match_firmware(self):
        import re
        source = (Path(__file__).resolve().parents[1] / "main/projector_logic.c").read_text()
        block = re.search(r"projector_message_levels\[MESSAGE_LEVEL_COUNT\] = \{(.*?)\};", source, re.S).group(1)
        const = importlib.import_module("bed_projector.const")
        self.assertEqual(tuple(re.findall(r'"([^"]+)"', block)), const.MESSAGE_LEVELS)


if __name__ == "__main__":
    unittest.main()
