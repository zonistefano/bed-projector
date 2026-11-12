"""Page navigation and alarm buttons."""

from __future__ import annotations

from homeassistant.components.button import ButtonEntity

from .entity import ProjectorEntity


async def async_setup_entry(hass, entry, async_add_entities):
    coordinator = entry.runtime_data
    async_add_entities([
        ProjectorPageButton(coordinator, "previous", "Pagina precedente", "mdi:chevron-left"),
        ProjectorPageButton(coordinator, "next", "Pagina successiva", "mdi:chevron-right"),
        ProjectorDismissWake(coordinator),
    ])


class ProjectorPageButton(ProjectorEntity, ButtonEntity):
    def __init__(self, coordinator, direction, name, icon):
        super().__init__(coordinator, direction, name)
        self.direction = direction
        self._attr_icon = icon

    async def async_press(self):
        await self.coordinator.command({"move": self.direction})


class ProjectorDismissWake(ProjectorEntity, ButtonEntity):
    """Stop the sunrise or alarm under way, e.g. from an automation when the phone alarm is dismissed."""

    _attr_icon = "mdi:alarm-off"

    def __init__(self, coordinator):
        super().__init__(coordinator, "dismiss_wake", "Ferma sveglia")

    async def async_press(self):
        await self.coordinator.wake({"dismiss": True})
