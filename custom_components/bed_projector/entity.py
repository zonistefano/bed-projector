"""Shared entity identity for the Bed Projector."""

from homeassistant.helpers.device_registry import DeviceInfo
from homeassistant.helpers.update_coordinator import CoordinatorEntity

from .const import DOMAIN
from .coordinator import ProjectorCoordinator


class ProjectorEntity(CoordinatorEntity[ProjectorCoordinator]):
    _attr_has_entity_name = True

    def __init__(self, coordinator: ProjectorCoordinator, key: str, name: str) -> None:
        super().__init__(coordinator)
        self._attr_unique_id = f"{coordinator.entry.entry_id}_{key}"
        self._attr_name = name
        self._attr_device_info = DeviceInfo(
            identifiers={(DOMAIN, coordinator.entry.entry_id)},
            name=coordinator.entry.title,
            manufacturer="Bed Projector",
            model="ESP32-S3 projection clock",
            configuration_url=coordinator.base,  # Opens the device's web UI.
        )
