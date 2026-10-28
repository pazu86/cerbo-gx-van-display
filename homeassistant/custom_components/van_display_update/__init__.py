"""Firmware for the van display, served to it through Home Assistant.

The display asks GET /api/van_display/manifest?running=<version> a minute after it joins a network
and every 30 min after that. When a newer build is published it downloads
GET /api/van_display/firmware and installs it itself (OTA with automatic rollback). The image embeds
the display's WiFi and other credentials, so it must never go in the public /config/www: both
endpoints need the display's key (header X-Display-Key, `van_display_key` in secrets.yaml, also in
the display's secrets.h) or a Home Assistant admin's login.

Publish a build by copying it to /config/van_display/firmware.bin (`tools\\build.ps1 ha`).
The version comes from the image itself.

configuration.yaml:

    van_display_update:
      key: !secret van_display_key
"""

from __future__ import annotations

from dataclasses import dataclass
from datetime import datetime
import hashlib
import hmac
import logging
import os
import struct

from aiohttp import web
import voluptuous as vol

from homeassistant.components.http import KEY_HASS, HomeAssistantView
from homeassistant.core import HomeAssistant
from homeassistant.helpers import config_validation as cv, discovery
from homeassistant.helpers.dispatcher import async_dispatcher_send
from homeassistant.helpers.typing import ConfigType
from homeassistant.util import dt as dt_util

_LOGGER = logging.getLogger(__name__)

DOMAIN = "van_display_update"
SLUG = "van_display"        # URL path and folder under /config
PROJECT = "van_display"     # project name the image must carry (project() in firmware/CMakeLists.txt)
CONF_KEY = "key"
SIGNAL = f"{DOMAIN}_changed"

CONFIG_SCHEMA = vol.Schema(
    {DOMAIN: vol.Schema({vol.Required(CONF_KEY): vol.All(cv.string, vol.Length(min=32))})},
    extra=vol.ALLOW_EXTRA,
)


@dataclass
class Build:
    version: str
    size: int
    sha256: str
    built: str
    mtime: float


@dataclass
class State:
    path: str
    key: str
    build: Build | None = None
    error: str | None = None
    installed: str | None = None  # last version the display reported running
    last_check: datetime | None = None


def read_build(path: str) -> Build | None:
    """Version, size and SHA-256 of an ESP32 app image (None if there is no file)."""
    try:
        st = os.stat(path)
    except FileNotFoundError:
        return None
    with open(path, "rb") as f:
        head = f.read(256)
    # 24-byte image header (magic 0xE9) + 8-byte first segment header, then esp_app_desc_t:
    # magic 0xABCD5432 at +0, version[32] at +16, project_name[32] at +48.
    if len(head) < 256 or head[0] != 0xE9 or struct.unpack_from("<I", head, 32)[0] != 0xABCD5432:
        raise ValueError("not an ESP32 app image")

    def text(offset: int, size: int) -> str:
        return head[offset : offset + size].split(b"\0")[0].decode(errors="replace")

    image_project = text(80, 32)
    if image_project != PROJECT:
        raise ValueError(f"image is for {image_project!r}, not {PROJECT!r}")
    digest = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 16), b""):
            digest.update(chunk)
    # "built" = when it was published here: the image's own date/time is only its compile time.
    published = dt_util.as_local(dt_util.utc_from_timestamp(st.st_mtime)).strftime("%Y-%m-%d %H:%M")
    return Build(text(48, 32), st.st_size, digest.hexdigest(), published, st.st_mtime)


async def async_refresh(hass: HomeAssistant, state: State) -> State:
    """Re-read the published image when it has changed."""

    def changed() -> bool:
        try:
            mtime = os.stat(state.path).st_mtime
        except FileNotFoundError:
            return state.build is not None or state.error is not None
        return state.build is None or state.build.mtime != mtime

    if await hass.async_add_executor_job(changed):
        try:
            state.build = await hass.async_add_executor_job(read_build, state.path)
            state.error = None
            if state.build:
                _LOGGER.info("Van display firmware %s published (%d bytes)", state.build.version, state.build.size)
        except (OSError, ValueError) as err:
            state.build, state.error = None, str(err)
            _LOGGER.error("Van display firmware at %s not usable: %s", state.path, err)
        async_dispatcher_send(hass, SIGNAL)
    return state


def allowed(request: web.Request, state: State) -> bool:
    user = request.get("hass_user")
    if user and user.is_admin:
        return True
    key = request.headers.get("X-Display-Key", "")
    return bool(key and hmac.compare_digest(key.encode(), state.key.encode()))


class DisplayView(HomeAssistantView):
    """Base for the two endpoints. The display has no HA user: they check its key (or an admin)."""

    requires_auth = False

    def __init__(self, state: State, what: str) -> None:
        self.state = state
        self.url = f"/api/{SLUG}/{what}"
        self.name = f"api:{SLUG}:{what}"


class ManifestView(DisplayView):
    """What's published, and a note of what the display is running."""

    def __init__(self, state: State) -> None:
        super().__init__(state, "manifest")

    async def get(self, request: web.Request) -> web.Response:
        hass = request.app[KEY_HASS]
        state = await async_refresh(hass, self.state)
        if not allowed(request, state):
            return self.json_message("not allowed", 403)
        running = request.query.get("running", "")[:31]
        if running:
            state.installed = running
            state.last_check = dt_util.utcnow()
            async_dispatcher_send(hass, SIGNAL)
        b = state.build
        if not b:
            return self.json({})
        return self.json({"version": b.version, "size": b.size, "sha256": b.sha256, "built": b.built,
                          "url": f"/api/{SLUG}/firmware"})


class FirmwareView(DisplayView):
    """The published image."""

    def __init__(self, state: State) -> None:
        super().__init__(state, "firmware")

    async def get(self, request: web.Request) -> web.StreamResponse:
        hass = request.app[KEY_HASS]
        state = await async_refresh(hass, self.state)
        if not allowed(request, state):
            return self.json_message("not allowed", 403)
        if not state.build:
            return self.json_message("no firmware published", 404)
        _LOGGER.info("Van display firmware %s downloaded", state.build.version)
        return web.FileResponse(state.path, headers={"Content-Type": "application/octet-stream"})


async def async_setup(hass: HomeAssistant, config: ConfigType) -> bool:
    state = State(hass.config.path(SLUG, "firmware.bin"), config[DOMAIN][CONF_KEY])
    await async_refresh(hass, state)
    hass.http.register_view(ManifestView(state))
    hass.http.register_view(FirmwareView(state))
    hass.data[DOMAIN] = state
    hass.async_create_task(discovery.async_load_platform(hass, "update", DOMAIN, {}, config))
    return True
