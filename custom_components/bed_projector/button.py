"""Page navigation buttons."""

from homeassistant.components.button import ButtonEntity

from .entity import ProjectorEntity


async def async_setup_entry(hass, entry, async_add_entities):
    coordinator = entry.runtime_data
    async_add_entities([
        ProjectorPageButton(coordinator, "previous", "Pagina precedente", "mdi:chevron-left"),
        ProjectorPageButton(coordinator, "next", "Pagina successiva", "mdi:chevron-right"),
    ])


class ProjectorPageButton(ProjectorEntity, ButtonEntity):
    def __init__(self, coordinator, direction, name, icon):
        super().__init__(coordinator, direction, name)
        self.direction = direction
        self._attr_icon = icon

    async def async_press(self):
        await self.coordinator.command({"move": self.direction})
