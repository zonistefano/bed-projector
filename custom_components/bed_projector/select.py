"""Page selection entity."""

from homeassistant.components.select import SelectEntity

from .const import DOMAIN
from .entity import ProjectorEntity


async def async_setup_entry(hass, entry, async_add_entities):
    async_add_entities([ProjectorPage(hass.data[DOMAIN][entry.entry_id])])


class ProjectorPage(ProjectorEntity, SelectEntity):
    def __init__(self, coordinator):
        super().__init__(coordinator, "page", "Pagina")

    @property
    def options(self):
        return [page["id"] for page in self.coordinator.data.get("pages", [])]

    @property
    def current_option(self):
        return self.coordinator.data.get("page")

    async def async_select_option(self, option):
        if option not in self.options:
            raise ValueError("Unknown page")
        await self.coordinator.command({"page": option})
