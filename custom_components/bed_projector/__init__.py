"""Bed Projector integration: device controls and HA state forwarding."""

from datetime import timedelta
from homeassistant.core import HomeAssistant, callback
from homeassistant.helpers.event import async_track_state_change_event, async_track_time_interval

from .const import DOMAIN, PLATFORMS
from .coordinator import ProjectorCoordinator


async def async_setup_entry(hass: HomeAssistant, entry) -> bool:
    coordinator = ProjectorCoordinator(hass, entry)
    await coordinator.async_config_entry_first_refresh()
    hass.data.setdefault(DOMAIN, {})[entry.entry_id] = coordinator

    @callback
    def changed(_event) -> None:
        coordinator.request_push()

    entities = coordinator.watched_entities()
    if entities:
        entry.async_on_unload(async_track_state_change_event(hass, entities, changed))
    entry.async_on_unload(async_track_time_interval(hass, changed, timedelta(seconds=60)))
    entry.async_on_unload(entry.add_update_listener(_async_options_updated))
    await hass.config_entries.async_forward_entry_setups(entry, PLATFORMS)
    coordinator.request_push()
    return True


async def _async_options_updated(hass: HomeAssistant, entry) -> None:
    await hass.config_entries.async_reload(entry.entry_id)


async def async_unload_entry(hass: HomeAssistant, entry) -> bool:
    coordinator = hass.data[DOMAIN][entry.entry_id]
    unloaded = await hass.config_entries.async_unload_platforms(entry, PLATFORMS)
    if unloaded:
        await coordinator.async_stop()
        hass.data[DOMAIN].pop(entry.entry_id)
    return unloaded
