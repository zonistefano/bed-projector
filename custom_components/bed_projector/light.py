"""Projector lamp and brightness control."""

from homeassistant.components.light import ATTR_BRIGHTNESS, ColorMode, LightEntity

from .entity import ProjectorEntity


async def async_setup_entry(hass, entry, async_add_entities):
    async_add_entities([ProjectorLight(entry.runtime_data)])


class ProjectorLight(ProjectorEntity, LightEntity):
    _attr_supported_color_modes = {ColorMode.BRIGHTNESS}
    _attr_color_mode = ColorMode.BRIGHTNESS

    def __init__(self, coordinator):
        super().__init__(coordinator, "light", "Proiezione")

    @property
    def is_on(self):
        return self.coordinator.data["power"]

    @property
    def brightness(self):
        return round(self.coordinator.data["brightness"] * 255 / 100)

    async def async_turn_on(self, **kwargs):
        if kwargs.get(ATTR_BRIGHTNESS) == 0:
            await self.async_turn_off()
            return
        payload = {"power": True}
        if ATTR_BRIGHTNESS in kwargs:
            payload["brightness"] = max(1, min(100, round(kwargs[ATTR_BRIGHTNESS] * 100 / 255)))
        await self.coordinator.command(payload)

    async def async_turn_off(self, **kwargs):
        await self.coordinator.command({"power": False})
