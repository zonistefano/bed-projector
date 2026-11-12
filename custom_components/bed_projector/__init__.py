"""Bed Projector integration: device controls and HA state forwarding."""

from __future__ import annotations

from datetime import timedelta

import voluptuous as vol
from homeassistant.config_entries import ConfigEntry, ConfigEntryState
from homeassistant.const import EVENT_STATE_CHANGED
from homeassistant.core import Event, HomeAssistant, ServiceCall, callback
from homeassistant.exceptions import ServiceValidationError
from homeassistant.helpers import config_validation as cv
from homeassistant.helpers.event import (TrackTemplate, async_track_state_change_event,
                                         async_track_template_result, async_track_time_interval)
from homeassistant.helpers.typing import ConfigType

from .const import (DOMAIN, MESSAGE_DURATION_DEFAULT, MESSAGE_DURATION_RANGE, MESSAGE_ICONS,
                    MESSAGE_LEVEL_DEFAULT, MESSAGE_LEVELS, PLATFORMS)
from .coordinator import ProjectorCoordinator
from .protocol import message_payload

# The firmware forgets HA data after 120 s; a full snapshot every minute keeps it fresh.
FULL_PUSH_INTERVAL = timedelta(seconds=60)

type BedProjectorConfigEntry = ConfigEntry[ProjectorCoordinator]

CONFIG_SCHEMA = cv.config_entry_only_config_schema(DOMAIN)
ATTR_CONFIG_ENTRY_ID = "config_entry_id"
ATTR_MESSAGE = "message"
ATTR_ICON = "icon"
ATTR_DURATION = "duration"
ATTR_LEVEL = "level"
ATTR_BLINK = "blink"
SHOW_MESSAGE_SCHEMA = vol.Schema({
    vol.Required(ATTR_CONFIG_ENTRY_ID): cv.string,
    vol.Required(ATTR_MESSAGE): cv.string,
    vol.Optional(ATTR_ICON): vol.In(MESSAGE_ICONS),
    # 0 keeps the message until clear_message.
    vol.Optional(ATTR_DURATION, default=MESSAGE_DURATION_DEFAULT):
        vol.All(vol.Coerce(int), vol.Any(0, vol.Range(*MESSAGE_DURATION_RANGE))),
    vol.Optional(ATTR_LEVEL, default=MESSAGE_LEVEL_DEFAULT): vol.In(MESSAGE_LEVELS),
    vol.Optional(ATTR_BLINK, default=False): cv.boolean,
})
CLEAR_MESSAGE_SCHEMA = vol.Schema({vol.Required(ATTR_CONFIG_ENTRY_ID): cv.string})


def _coordinator(hass: HomeAssistant, call: ServiceCall) -> ProjectorCoordinator:
    entry = hass.config_entries.async_get_entry(call.data[ATTR_CONFIG_ENTRY_ID])
    if entry is None or entry.domain != DOMAIN or entry.state is not ConfigEntryState.LOADED:
        raise ServiceValidationError("Bed Projector entry not found or not loaded")
    return entry.runtime_data


async def async_setup(hass: HomeAssistant, config: ConfigType) -> bool:
    """Actions that show a temporary message over every page."""

    async def show_message(call: ServiceCall) -> None:
        coordinator = _coordinator(hass, call)
        try:
            payload = message_payload(call.data[ATTR_MESSAGE], call.data.get(ATTR_ICON),
                                      call.data[ATTR_DURATION], call.data[ATTR_LEVEL],
                                      call.data[ATTR_BLINK])
        except ValueError as error:
            raise ServiceValidationError(f"Invalid message: {error}") from error
        await coordinator.message(payload)

    async def clear_message(call: ServiceCall) -> None:
        await _coordinator(hass, call).message({"clear": True})

    hass.services.async_register(DOMAIN, "show_message", show_message, schema=SHOW_MESSAGE_SCHEMA)
    hass.services.async_register(DOMAIN, "clear_message", clear_message, schema=CLEAR_MESSAGE_SCHEMA)
    return True


async def async_setup_entry(hass: HomeAssistant, entry: BedProjectorConfigEntry) -> bool:
    coordinator = ProjectorCoordinator(hass, entry)
    await coordinator.async_config_entry_first_refresh()
    entry.runtime_data = coordinator

    @callback
    def changed(_now) -> None:
        coordinator.request_push()

    @callback
    def entity_changed(event: Event) -> None:
        coordinator.entity_changed(event.data["entity_id"])

    entities = coordinator.watched_entities()
    if entities:
        entry.async_on_unload(async_track_state_change_event(hass, entities, entity_changed))

    @callback
    def listed_changed(event: Event) -> None:
        # Doors, windows and lights named on the display, including discovered ones.
        old, new = event.data.get("old_state"), event.data.get("new_state")
        if (old is None or new is None or old.state != new.state) and \
                coordinator.listed(new or old):
            coordinator.request_push()

    entry.async_on_unload(hass.bus.async_listen(EVENT_STATE_CHANGED, listed_changed))
    if coordinator.templates:
        # Name templates: HA re-renders them when the entities they read change.
        @callback
        def template_changed(_event, _updates) -> None:
            coordinator.request_push()

        tracker = async_track_template_result(
            hass, [TrackTemplate(template, None) for template in coordinator.templates.values()],
            template_changed)
        entry.async_on_unload(tracker.async_remove)
    entry.async_on_unload(async_track_time_interval(hass, changed, FULL_PUSH_INTERVAL))
    entry.async_on_unload(entry.add_update_listener(_async_options_updated))
    await hass.config_entries.async_forward_entry_setups(entry, PLATFORMS)
    coordinator.load_own_entities()
    coordinator.request_push()
    return True


async def _async_options_updated(hass: HomeAssistant, entry: BedProjectorConfigEntry) -> None:
    await hass.config_entries.async_reload(entry.entry_id)


async def async_unload_entry(hass: HomeAssistant, entry: BedProjectorConfigEntry) -> bool:
    unloaded = await hass.config_entries.async_unload_platforms(entry, PLATFORMS)
    if unloaded:
        await entry.runtime_data.async_stop()
    return unloaded
