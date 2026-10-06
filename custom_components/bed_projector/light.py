"""Projector lamp and brightness control."""

from homeassistant.components.light import ColorMode, LightEntity

from .const import DOMAIN
from .entity import ProjectorEntity


async def async_setup_entry(hass, entry, async_add_entities):
    async_add_entities([ProjectorLight(hass.data[DOMAIN][entry.entry_id])])


class ProjectorLight(ProjectorEntity, LightEntity):
    _attr_supported_color_modes = {ColorMode.BRIGHTNESS}

    def __init__(self, coordinator):
        super().__init__(coordinator, "light", "Proiezione")

    @property
    def is_on(self):
        return self.coordinator.data["power"]

    @property
    def color_mode(self):
        return ColorMode.BRIGHTNESS

    @property
    def brightness(self):
        return round(self.coordinator.data["brightness"] * 255 / 100)

    async def async_turn_on(self, **kwargs):
        if kwargs.get("brightness") == 0:
            await self.coordinator.command({"power": False})
            return
        payload = {"power": True}
        if "brightness" in kwargs:
            payload["brightness"] = max(1, min(100, round(kwargs["brightness"] * 100 / 255)))
        await self.coordinator.command(payload)

    async def async_turn_off(self, **kwargs):
        await self.coordinator.command({"power": False})
