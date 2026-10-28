"""Update entity: the version the display runs vs the one published for it.

The display installs updates itself (it pulls them), so there's no install action here.
"""

from __future__ import annotations

from datetime import timedelta
from typing import Any

from homeassistant.components.update import UpdateEntity, UpdateEntityFeature
from homeassistant.core import HomeAssistant
from homeassistant.helpers.dispatcher import async_dispatcher_connect
from homeassistant.helpers.entity_platform import AddEntitiesCallback
from homeassistant.helpers.event import async_track_time_interval
from homeassistant.helpers.restore_state import RestoreEntity
from homeassistant.helpers.typing import ConfigType, DiscoveryInfoType
from homeassistant.util import dt as dt_util

from . import DOMAIN, SIGNAL, State, async_refresh


async def async_setup_platform(hass: HomeAssistant, config: ConfigType, async_add_entities: AddEntitiesCallback,
                               discovery_info: DiscoveryInfoType | None = None) -> None:
    if discovery_info is not None:
        async_add_entities([VanDisplayFirmware(hass.data[DOMAIN])])


class VanDisplayFirmware(UpdateEntity, RestoreEntity):
    _attr_has_entity_name = False
    _attr_supported_features = UpdateEntityFeature(0)
    _attr_should_poll = False
    _attr_name = "Van display firmware"
    _attr_unique_id = "van_display_firmware"
    _attr_title = "Van display"
    _attr_icon = "mdi:caravan"

    def __init__(self, state: State) -> None:
        self._state = state

    async def async_added_to_hass(self) -> None:
        if self._state.installed is None and (last := await self.async_get_last_state()):
            self._state.installed = last.attributes.get("installed_version")
            if checked := last.attributes.get("last_check"):
                self._state.last_check = dt_util.parse_datetime(checked)
        self.async_on_remove(async_dispatcher_connect(self.hass, SIGNAL, self.async_write_ha_state))

        async def refresh(_now) -> None:  # notice a newly published file without a display check
            await async_refresh(self.hass, self._state)

        self.async_on_remove(async_track_time_interval(self.hass, refresh, timedelta(minutes=5)))

    @property
    def installed_version(self) -> str | None:
        return self._state.installed

    @property
    def latest_version(self) -> str | None:
        b = self._state.build
        return b.version if b else self._state.installed

    @property
    def release_summary(self) -> str | None:
        if self._state.error:
            return f"Published file not usable: {self._state.error}"
        b = self._state.build
        if b and self._state.installed and b.version != self._state.installed:
            return (f"Published {b.built}. The display installs it itself at its next check (a minute after it "
                    "joins a network, then every 30 min, not while the van is on the move).")
        return None

    @property
    def extra_state_attributes(self) -> dict[str, Any]:
        b = self._state.build
        return {
            "last_check": self._state.last_check.isoformat() if self._state.last_check else None,
            "published": b.built if b else None,
            "size": b.size if b else None,
            "sha256": b.sha256 if b else None,
            "error": self._state.error,
        }
