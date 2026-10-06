"""Page navigation buttons."""

from homeassistant.components.button import ButtonEntity

from .const import DOMAIN
from .entity import ProjectorEntity


async def async_setup_entry(hass, entry, async_add_entities):
    coordinator = hass.data[DOMAIN][entry.entry_id]
    async_add_entities([
        ProjectorPageButton(coordinator, "previous", "Pagina precedente"),
        ProjectorPageButton(coordinator, "next", "Pagina successiva"),
    ])


class ProjectorPageButton(ProjectorEntity, ButtonEntity):
    def __init__(self, coordinator, direction, name):
        super().__init__(coordinator, direction, name)
        self.direction = direction

    async def async_press(self):
        await self.coordinator.command({"move": self.direction})
