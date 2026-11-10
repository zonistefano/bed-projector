"""Local HTTP client and state synchronization for Bed Projector."""

from __future__ import annotations

import asyncio
import logging
import time
from datetime import timedelta
from typing import Any

from aiohttp import ClientError, ClientTimeout
from homeassistant.const import SUN_EVENT_SUNRISE, SUN_EVENT_SUNSET
from homeassistant.core import HomeAssistant
from homeassistant.exceptions import HomeAssistantError, TemplateError
from homeassistant.helpers import entity_registry as er
from homeassistant.helpers.aiohttp_client import async_get_clientsession
from homeassistant.helpers.sun import get_astral_event_date
from homeassistant.helpers.template import Template
from homeassistant.helpers.update_coordinator import DataUpdateCoordinator, UpdateFailed
from homeassistant.util import dt as dt_util

from .const import (CONF_CALENDARS, CONF_LIGHT_LIST, CONF_LIGHT_TEMPLATE, CONF_OPENING_LIST,
                    CONF_OPENING_TEMPLATE, CONF_SINGLE, CONF_TOKEN, CONF_WEATHER, DOMAIN,
                    OPENING_CLASSES, SEASON_AUTO)
from .protocol import FORECAST_DAYS, make_snapshot, template_names

_LOGGER = logging.getLogger(__name__)
# Forecasts and calendars change slowly; state pushes happen on every watched change.
FORECAST_TTL = 600
EVENTS_TTL = 600
FORECAST_TYPES = ("daily", "hourly", "twice_daily")
# Name lists by domain: (entity list option, template option).
NAME_SOURCES = {"binary_sensor": (CONF_OPENING_LIST, CONF_OPENING_TEMPLATE),
                "light": (CONF_LIGHT_LIST, CONF_LIGHT_TEMPLATE)}


class ProjectorCoordinator(DataUpdateCoordinator[dict[str, Any]]):
    """Poll device status and push selected HA entities to the display."""

    def __init__(self, hass: HomeAssistant, entry) -> None:
        super().__init__(hass, logger=_LOGGER, name=DOMAIN, update_interval=timedelta(seconds=30))
        self.entry = entry
        self.base = f"http://{entry.data['host']}:{entry.data.get('port', 80)}"
        self.headers = {"Authorization": f"Bearer {entry.data[CONF_TOKEN]}"}
        self.push_task: asyncio.Task | None = None
        self.push_dirty = False
        self.forecasts: dict[str, list[dict[str, Any]] | None] = {}
        self.forecasts_at: float | None = None
        self.events: list[dict[str, Any]] | None = None
        self.events_at: float | None = None
        # Thermostat season, set by the select entity (restored across restarts).
        self.season = SEASON_AUTO
        # This integration's entities never change while the entry is loaded.
        self.own_entities: set[str] = set()
        # Domains whose names come from a template, tracked by async_track_template_result.
        self.templates: dict[str, Template] = {}
        for domain, (_, key) in NAME_SOURCES.items():
            text = entry.options.get(key)
            if isinstance(text, str) and text.strip():
                self.templates[domain] = Template(text, hass)

    async def request(self, method: str, path: str, payload: dict | None = None) -> dict:
        session = async_get_clientsession(self.hass)
        try:
            async with session.request(method, self.base + path, headers=self.headers,
                                       json=payload, timeout=ClientTimeout(total=8)) as response:
                data = await response.json(content_type=None)
                if response.status >= 400:
                    error = data.get("error", "") if isinstance(data, dict) else ""
                    raise UpdateFailed(f"Device HTTP {response.status}: {error}")
                return data
        except (ClientError, asyncio.TimeoutError, ValueError) as error:
            raise UpdateFailed(f"Device unreachable: {error}") from error

    async def _async_update_data(self) -> dict[str, Any]:
        status, pages = await asyncio.gather(
            self.request("GET", "/api/status"),
            self.request("GET", "/api/pages"),
        )
        status["pages"] = pages.get("pages", [])
        return status

    async def command(self, payload: dict) -> None:
        """Send a display command from an entity action; failures reach the user."""
        try:
            status = await self.request("POST", "/api/display", payload)
        except UpdateFailed as error:
            raise HomeAssistantError(f"Bed Projector: {error}") from error
        if self.data is not None:
            # The device answers with its new status: show it without another poll.
            self.async_set_updated_data({**self.data, **status})

    async def message(self, payload: dict) -> None:
        """Show or clear a message from a user action; failures reach the user."""
        try:
            await self.request("POST", "/api/message", payload)
        except UpdateFailed as error:
            raise HomeAssistantError(f"Bed Projector: {error}") from error

    def page_labels(self) -> dict[str, str]:
        """Page id -> name shown to the user; ids stand in for duplicate names."""
        pages = (self.data or {}).get("pages", [])
        names = [page.get("name") or page["id"] for page in pages]
        return {page["id"]: name if names.count(name) == 1 else page["id"]
                for page, name in zip(pages, names)}

    def watched_entities(self) -> list[str]:
        options = self.entry.options
        values = [options.get(key) for key in CONF_SINGLE]
        values += [*self._list(CONF_OPENING_LIST), *self._list(CONF_LIGHT_LIST),
                   *self._list(CONF_CALENDARS)]
        return list(dict.fromkeys(value for value in values if isinstance(value, str) and value))

    def _list(self, key: str) -> list[str]:
        value = self.entry.options.get(key)
        return [item for item in value if isinstance(item, str)] if isinstance(value, list) else []

    def entity_changed(self, entity_id: str) -> None:
        """Push a watched change; a calendar change also refetches its events."""
        if entity_id in self._list(CONF_CALENDARS):
            self.events_at = None
        self.request_push()

    def load_own_entities(self) -> None:
        """Remember this integration's entities, such as the projector's own light."""
        registry = er.async_get(self.hass)
        self.own_entities = {entry.entity_id for entry in
                             er.async_entries_for_config_entry(registry, self.entry.entry_id)}

    def listed(self, state) -> bool:
        """Whether a state belongs to the door/window or light name lists."""
        # Runs on every state change in HA: cheap checks first.
        if state is None or state.domain not in NAME_SOURCES or state.domain in self.templates or \
                state.entity_id in self.own_entities:
            return False
        domain = state.domain
        configured = self._list(NAME_SOURCES[domain][0])
        if configured:
            return state.entity_id in configured
        if domain == "binary_sensor":
            return state.attributes.get("device_class") in OPENING_CLASSES
        return "entity_id" not in state.attributes  # Groups would repeat their members.

    def _names_on(self, domain: str) -> list[str] | None:
        """Names rendered by the domain's template (None if it fails), else friendly names
        of the listed entities that are on; without a configured list, every door/window
        contact or every single light but the projector."""
        template = self.templates.get(domain)
        if template is not None:
            try:
                return template_names(template.async_render())
            except TemplateError as error:
                _LOGGER.warning("Name template for %s failed: %s", domain, error)
                return None
        return sorted(state.name for state in self.hass.states.async_all(domain)
                      if state.state == "on" and self.listed(state))

    def request_push(self) -> None:
        self.push_dirty = True
        if self.push_task is None or self.push_task.done():
            self.push_task = self.hass.async_create_task(self._push_loop())

    async def _push_loop(self) -> None:
        while self.push_dirty:
            self.push_dirty = False
            await asyncio.sleep(0.2)  # Coalesce bursts of state changes.
            try:
                await self.request("POST", "/api/ha/state", await self._snapshot())
            except UpdateFailed as error:
                # A later event or the periodic full snapshot will retry.
                _LOGGER.debug("State push failed: %s", error)

    async def _snapshot(self) -> dict[str, Any]:
        states = {entity_id: self.hass.states.get(entity_id) for entity_id in self.watched_entities()}
        forecasts = await self._forecasts()
        now = dt_util.now()
        events = await self._events(now)
        today = now.date()
        sun = {}
        for offset in range(FORECAST_DAYS):
            day = today + timedelta(days=offset)
            sun[day] = (get_astral_event_date(self.hass, SUN_EVENT_SUNRISE, day),
                        get_astral_event_date(self.hass, SUN_EVENT_SUNSET, day))
        return make_snapshot(
            states, self.entry.options, daily=forecasts.get("daily"),
            hourly=forecasts.get("hourly"), twice_daily=forecasts.get("twice_daily"),
            today=today, tz=now.tzinfo, sun=sun,
            opening_names=self._names_on("binary_sensor"),
            light_names=self._names_on("light"), now=now, events=events, season=self.season)

    async def _forecasts(self) -> dict[str, list[dict[str, Any]] | None]:
        """Daily, hourly and twice-daily forecasts, cached for FORECAST_TTL seconds.

        Weather entities expose forecasts only through weather.get_forecasts. Empty
        results are cached too, so providers without forecasts are not asked on every push.
        """
        entity_id = self.entry.options.get(CONF_WEATHER)
        if not isinstance(entity_id, str) or not entity_id:
            return {}
        if self.forecasts_at is not None and time.monotonic() - self.forecasts_at < FORECAST_TTL:
            return self.forecasts

        async def fetch(kind: str) -> list[dict[str, Any]] | None:
            try:
                response = await self.hass.services.async_call(
                    "weather", "get_forecasts", {"entity_id": entity_id, "type": kind},
                    blocking=True, return_response=True)
            except HomeAssistantError:
                return None  # The provider does not support this forecast type.
            return (response or {}).get(entity_id, {}).get("forecast") or None

        forecasts = dict(zip(FORECAST_TYPES, await asyncio.gather(*map(fetch, FORECAST_TYPES))))
        if not forecasts["daily"] and forecasts["twice_daily"]:
            # Providers without a daily forecast: daytime entries stand in for days.
            forecasts["daily"] = [item for item in forecasts["twice_daily"] if item.get("is_daytime")]
        self.forecasts, self.forecasts_at = forecasts, time.monotonic()
        return forecasts

    async def _events(self, now) -> list[dict[str, Any]] | None:
        """Events of the configured calendars until the end of tomorrow, cached for
        EVENTS_TTL seconds; None without calendars or when calendar.get_events fails."""
        calendars = self._list(CONF_CALENDARS)
        if not calendars:
            return None
        if self.events_at is not None and time.monotonic() - self.events_at < EVENTS_TTL:
            return self.events
        end = dt_util.start_of_local_day(now.date() + timedelta(days=2))
        try:
            response = await self.hass.services.async_call(
                "calendar", "get_events",
                {"entity_id": calendars, "start_date_time": now, "end_date_time": end},
                blocking=True, return_response=True)
        except HomeAssistantError as error:
            _LOGGER.warning("Calendar events unavailable: %s", error)
            self.events = None
        else:
            self.events = [item for result in (response or {}).values()
                           for item in (result or {}).get("events", [])]
        self.events_at = time.monotonic()
        return self.events

    async def async_stop(self) -> None:
        if self.push_task and not self.push_task.done():
            self.push_task.cancel()
            try:
                await self.push_task
            except asyncio.CancelledError:
                pass
