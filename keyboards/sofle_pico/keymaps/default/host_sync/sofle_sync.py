#!/usr/bin/env python3
"""
Cross-platform (Linux + Windows) Raw HID host sync bridge for Dual-USB Sofle Pico splits.

Relays low-latency layer (`_LOWER` / `_RAISE` / `_ADJUST`), modifier (OLED badges),
lights toggle, and RGB Matrix state between the Left and Right halves when connected
via two USB cables without a TRRS interconnect. Leaves standard VIA Raw HID traffic
untouched.
"""

from __future__ import annotations

import argparse
import ctypes
import glob
import json
import os
import select
import struct
import sys
import threading
import time
import urllib.error
import urllib.parse
import urllib.request
import uuid
from dataclasses import dataclass, field
from typing import Callable, Dict, List, Optional, Tuple

VID = 0x4A54
PID = 0x5350
RAW_USAGE_PAGE = 0xFF60
RAW_USAGE = 0x0061
RAW_EPSIZE = 32

CMD_SYNC = 0xF0
MAGIC = b"\x53\x50"  # "SP"
MSG_HOST_PING = 0x01
MSG_HALF_STATE = 0x02
MSG_IDENTIFY = 0x03
MSG_BOOTLOADER = 0x04
MSG_HOST_ACTION = 0x05
HOST_ACTION_HA_HEX_TOGGLE = 0x01
HOST_ACTION_AUDIO_SWAP = 0x02
MSG_HOST_STATUS = 0x06
HOST_AUDIO_UNKNOWN = 0
HOST_AUDIO_HEADSET = 1
HOST_AUDIO_SPEAKER = 2
AUDIO_POLL_INTERVAL_S = 2.0
BOOTLOADER_MAGIC = b"BOOT"

DEFAULT_HA_URL = "https://home-assistant.zuul-diphda.ts.net"
DEFAULT_HA_WEBHOOK_ID = "toggle_hex_lights"
DEFAULT_HA_ENTITY_ID = "light.h6061"
DEFAULT_HA_TIMEOUT_S = 5.0
DEFAULT_HA_CONFIG_PATH = os.path.join(os.path.dirname(os.path.abspath(__file__)), "ha_config.json")

DEFAULT_AUDIO_DEVICES = ("Wireless Headset", "Speakers")
DEFAULT_AUDIO_ROLES = ("console", "multimedia")
DEFAULT_AUDIO_CONFIG_PATH = os.path.join(os.path.dirname(os.path.abspath(__file__)), "audio_config.json")

AUDIO_ROLE_MAP: Dict[str, int] = {
    "console": 0,         # eConsole (Default Device)
    "multimedia": 1,      # eMultimedia (Default Device)
    "communications": 2,  # eCommunications (Default Communications Device)
}

RESCAN_INTERVAL_S = 1.5
PING_INTERVAL_S = 0.75


def build_ping_packet(
    peer_connected: bool,
    request_state: bool = False,
    request_rgb_authoritative: bool = False,
) -> bytes:
    """Build a 32-byte MSG_HOST_PING report for the Sofle Pico firmware."""
    pkt = bytearray(RAW_EPSIZE)
    pkt[0] = CMD_SYNC
    pkt[1:3] = MAGIC
    pkt[3] = MSG_HOST_PING
    pkt[4] = 1 if peer_connected else 0
    pkt[5] = 1 if request_state else 0
    pkt[6] = 1 if request_rgb_authoritative else 0
    return bytes(pkt)


def build_host_status_packet(audio_state: int) -> bytes:
    """Build a 32-byte MSG_HOST_STATUS report pushed from host daemon to keyboard halves."""
    pkt = bytearray(RAW_EPSIZE)
    pkt[0] = CMD_SYNC
    pkt[1:3] = MAGIC
    pkt[3] = MSG_HOST_STATUS
    pkt[4] = audio_state & 0xFF
    return bytes(pkt)


def normalize_raw_report(raw: bytes) -> Optional[bytes]:
    """Normalize a read HID report (32 bytes, or 33 bytes with leading 0x00 report ID)."""
    if len(raw) == RAW_EPSIZE:
        return raw
    if len(raw) == RAW_EPSIZE + 1 and raw[0] == 0:
        return raw[1:]
    return None


def parse_half_state(payload: bytes) -> Optional[str]:
    """Return 'left' or 'right' if payload is a valid MSG_HALF_STATE packet, else None."""
    if len(payload) != RAW_EPSIZE:
        return None
    if payload[0] != CMD_SYNC or payload[1:3] != MAGIC or payload[3] != MSG_HALF_STATE:
        return None
    return "left" if payload[4] != 0 else "right"


def parse_host_action(payload: bytes) -> Optional[Tuple[str, int, int]]:
    """Return (side, action_id, seq) if payload is a valid MSG_HOST_ACTION packet, else None."""
    if len(payload) != RAW_EPSIZE:
        return None
    if payload[0] != CMD_SYNC or payload[1:3] != MAGIC or payload[3] != MSG_HOST_ACTION:
        return None
    side = "left" if payload[4] != 0 else "right"
    return (side, payload[5], payload[6])


@dataclass
class HomeAssistantConfig:
    base_url: str = DEFAULT_HA_URL
    webhook_id: str = DEFAULT_HA_WEBHOOK_ID
    token: str = ""
    entity_id: str = DEFAULT_HA_ENTITY_ID
    timeout_s: float = DEFAULT_HA_TIMEOUT_S


def load_ha_config(
    config_path: Optional[str] = None,
    cli_url: Optional[str] = None,
    cli_webhook_id: Optional[str] = None,
    env: Optional[Dict[str, str]] = None,
) -> HomeAssistantConfig:
    """Load Home Assistant integration settings from defaults, JSON file, env vars, and CLI flags."""
    env_map: Dict[str, str] = os.environ if env is None else env  # type: ignore[assignment]
    path = config_path or env_map.get("SOFLE_HA_CONFIG") or DEFAULT_HA_CONFIG_PATH

    base_url = DEFAULT_HA_URL
    webhook_id = DEFAULT_HA_WEBHOOK_ID
    token = ""
    entity_id = DEFAULT_HA_ENTITY_ID
    timeout_s = DEFAULT_HA_TIMEOUT_S

    if path and os.path.isfile(path):
        try:
            with open(path, "r", encoding="utf-8") as f:
                raw_cfg = json.load(f)
            if isinstance(raw_cfg, dict):
                if "base_url" in raw_cfg and raw_cfg["base_url"] is not None:
                    base_url = str(raw_cfg["base_url"])
                if "webhook_id" in raw_cfg:
                    webhook_id = "" if raw_cfg["webhook_id"] is None else str(raw_cfg["webhook_id"])
                if "token" in raw_cfg and raw_cfg["token"] is not None:
                    token = str(raw_cfg["token"])
                if "entity_id" in raw_cfg and raw_cfg["entity_id"] is not None:
                    entity_id = str(raw_cfg["entity_id"])
                if "timeout_s" in raw_cfg and raw_cfg["timeout_s"] is not None:
                    try:
                        timeout_s = float(raw_cfg["timeout_s"])
                    except (TypeError, ValueError):
                        print(
                            f"[sofle-sync] Invalid timeout_s in {path}; using default",
                            file=sys.stderr,
                            flush=True,
                        )
            else:
                print(
                    f"[sofle-sync] Ignoring non-object Home Assistant config in {path}",
                    file=sys.stderr,
                    flush=True,
                )
        except (OSError, ValueError, TypeError) as exc:
            print(f"[sofle-sync] Failed to read Home Assistant config {path}: {exc}", file=sys.stderr, flush=True)

    if "SOFLE_HA_URL" in env_map:
        base_url = env_map["SOFLE_HA_URL"]
    if "SOFLE_HA_WEBHOOK_ID" in env_map:
        webhook_id = env_map["SOFLE_HA_WEBHOOK_ID"]
    if "SOFLE_HA_TOKEN" in env_map:
        token = env_map["SOFLE_HA_TOKEN"]
    if "SOFLE_HA_ENTITY_ID" in env_map:
        entity_id = env_map["SOFLE_HA_ENTITY_ID"]
    if "SOFLE_HA_TIMEOUT" in env_map:
        try:
            timeout_s = float(env_map["SOFLE_HA_TIMEOUT"])
        except (TypeError, ValueError):
            print("[sofle-sync] Invalid SOFLE_HA_TIMEOUT value; using previous timeout", file=sys.stderr, flush=True)

    if cli_url is not None:
        base_url = cli_url
    if cli_webhook_id is not None:
        webhook_id = cli_webhook_id

    base_url = base_url.strip().rstrip("/")
    webhook_id = webhook_id.strip()
    token = token.strip()
    entity_id = entity_id.strip() or DEFAULT_HA_ENTITY_ID
    timeout_s = max(0.5, float(timeout_s))

    return HomeAssistantConfig(
        base_url=base_url,
        webhook_id=webhook_id,
        token=token,
        entity_id=entity_id,
        timeout_s=timeout_s,
    )


def build_ha_request(cfg: HomeAssistantConfig, side: str) -> Tuple[urllib.request.Request, str]:
    """Build an HTTP POST request and sanitized display target for Home Assistant."""
    base_url = cfg.base_url.strip().rstrip("/")
    if not base_url:
        raise ValueError("Home Assistant base_url is not configured")

    webhook_id = cfg.webhook_id.strip()
    token = cfg.token.strip()
    entity_id = cfg.entity_id.strip() or DEFAULT_HA_ENTITY_ID

    headers: Dict[str, str] = {
        "Content-Type": "application/json",
        "User-Agent": "sofle-pico-sync/1.0",
    }

    if webhook_id:
        encoded_id = urllib.parse.quote(webhook_id, safe="")
        url = f"{base_url}/api/webhook/{encoded_id}"
        display_target = f"{base_url}/api/webhook/<configured>"
        body_obj: Dict[str, object] = {
            "source": "sofle_pico",
            "action": "toggle_hex_lights",
            "side": side,
            "entity_id": entity_id,
        }
    elif token:
        if entity_id.startswith("automation."):
            api_path = "/api/services/automation/trigger"
        else:
            domain = entity_id.split(".", 1)[0] if "." in entity_id else "homeassistant"
            api_path = f"/api/services/{urllib.parse.quote(domain, safe='')}/toggle"
        url = f"{base_url}{api_path}"
        display_target = url
        headers["Authorization"] = f"Bearer {token}"
        body_obj = {"entity_id": entity_id}
    else:
        raise ValueError("Home Assistant webhook_id or token not configured")

    data = json.dumps(body_obj).encode("utf-8")
    req = urllib.request.Request(url=url, data=data, headers=headers, method="POST")
    return req, display_target


class HomeAssistantActionDispatcher:
    """Dispatches host-side keyboard actions asynchronously to Home Assistant."""

    def __init__(
        self,
        config_path: Optional[str] = None,
        cli_url: Optional[str] = None,
        cli_webhook_id: Optional[str] = None,
        verbose: bool = False,
        opener: Optional[Callable[..., object]] = None,
        max_inflight: int = 4,
    ) -> None:
        self.config_path = config_path
        self.cli_url = cli_url
        self.cli_webhook_id = cli_webhook_id
        self.verbose = verbose
        self._opener = urllib.request.urlopen if opener is None else opener
        self._inflight = threading.Semaphore(max(1, max_inflight))

    def _log(self, msg: str) -> None:
        if self.verbose:
            print(f"[sofle-sync] {msg}", flush=True)

    def trigger_hex_toggle(self, side: str = "cli") -> bool:
        cfg = load_ha_config(
            config_path=self.config_path,
            cli_url=self.cli_url,
            cli_webhook_id=self.cli_webhook_id,
        )
        display_target = cfg.base_url or "<unconfigured>"
        try:
            req, display_target = build_ha_request(cfg, side=side)
        except ValueError as exc:
            print(f"[sofle-sync] Home Assistant config error ({display_target}): {exc}", file=sys.stderr, flush=True)
            return False

        try:
            with self._opener(req, timeout=cfg.timeout_s) as resp:  # type: ignore[attr-defined]
                status = getattr(resp, "status", None)
                if status is None and hasattr(resp, "getcode"):
                    status = resp.getcode()
                if status is None:
                    status = 200
                self._log(f"Home Assistant hex toggle ({side}) -> {display_target} HTTP {status}")
                return 200 <= int(status) < 300
        except urllib.error.HTTPError as exc:
            print(
                f"[sofle-sync] Home Assistant request failed ({side} -> {display_target}): HTTP {exc.code} {exc.reason}",
                file=sys.stderr,
                flush=True,
            )
            return False
        except (urllib.error.URLError, OSError, ValueError) as exc:
            print(
                f"[sofle-sync] Home Assistant request failed ({side} -> {display_target}): {exc}",
                file=sys.stderr,
                flush=True,
            )
            return False

    def _run_hex_toggle_worker(self, side: str) -> None:
        try:
            self.trigger_hex_toggle(side=side)
        finally:
            self._inflight.release()

    def dispatch_async(self, side: str, action_id: int, seq: int) -> None:
        if action_id != HOST_ACTION_HA_HEX_TOGGLE:
            self._log(f"Ignoring unknown host action id={action_id} from {side} (seq={seq})")
            return
        if not self._inflight.acquire(blocking=False):
            print(
                f"[sofle-sync] Dropping hex toggle from {side} (seq={seq}): too many requests in flight",
                file=sys.stderr,
                flush=True,
            )
            return
        t = threading.Thread(
            target=self._run_hex_toggle_worker,
            args=(side,),
            name=f"sofle-ha-{seq}",
            daemon=True,
        )
        t.start()


@dataclass(frozen=True)
class AudioEndpoint:
    device_id: str
    friendly_name: str
    device_desc: str = ""


@dataclass(frozen=True)
class AudioSwapConfig:
    devices: Tuple[str, ...] = DEFAULT_AUDIO_DEVICES
    roles: Tuple[str, ...] = DEFAULT_AUDIO_ROLES


def _parse_audio_csv(raw_val: str) -> Tuple[str, ...]:
    return tuple(item.strip() for item in raw_val.split(",") if item.strip())


def _normalize_audio_roles(raw_roles: object) -> Tuple[str, ...]:
    if isinstance(raw_roles, str):
        items = _parse_audio_csv(raw_roles)
    elif isinstance(raw_roles, (list, tuple)):
        items = tuple(str(x).strip() for x in raw_roles if str(x).strip())
    else:
        return DEFAULT_AUDIO_ROLES
    normalized: List[str] = []
    seen = set()
    for item in items:
        key = item.lower()
        if key in AUDIO_ROLE_MAP and key not in seen:
            normalized.append(key)
            seen.add(key)
    return tuple(normalized) if normalized else DEFAULT_AUDIO_ROLES


def load_audio_config(
    config_path: Optional[str] = None,
    cli_devices: Optional[str] = None,
    include_comms: bool = False,
    env: Optional[Dict[str, str]] = None,
) -> AudioSwapConfig:
    """Load Windows audio switcher settings from defaults, JSON file, env vars, and CLI flags."""
    env_map: Dict[str, str] = os.environ if env is None else env  # type: ignore[assignment]
    path = config_path or env_map.get("SOFLE_AUDIO_CONFIG") or DEFAULT_AUDIO_CONFIG_PATH

    devices: Tuple[str, ...] = DEFAULT_AUDIO_DEVICES
    roles: Tuple[str, ...] = DEFAULT_AUDIO_ROLES

    if path and os.path.isfile(path):
        try:
            with open(path, "r", encoding="utf-8") as f:
                raw_cfg = json.load(f)
            if isinstance(raw_cfg, dict):
                if "devices" in raw_cfg and raw_cfg["devices"] is not None:
                    raw_devs = raw_cfg["devices"]
                    if isinstance(raw_devs, list):
                        parsed_devs = tuple(str(d).strip() for d in raw_devs if str(d).strip())
                    elif isinstance(raw_devs, str):
                        parsed_devs = _parse_audio_csv(raw_devs)
                    else:
                        parsed_devs = ()
                    if parsed_devs:
                        devices = parsed_devs
                if "roles" in raw_cfg and raw_cfg["roles"] is not None:
                    roles = _normalize_audio_roles(raw_cfg["roles"])
            else:
                print(
                    f"[sofle-sync] Ignoring non-object audio config in {path}",
                    file=sys.stderr,
                    flush=True,
                )
        except (OSError, ValueError, TypeError) as exc:
            print(f"[sofle-sync] Failed to read audio config {path}: {exc}", file=sys.stderr, flush=True)

    if "SOFLE_AUDIO_DEVICES" in env_map:
        parsed_env_devs = _parse_audio_csv(env_map["SOFLE_AUDIO_DEVICES"])
        if parsed_env_devs:
            devices = parsed_env_devs
    if "SOFLE_AUDIO_ROLES" in env_map:
        roles = _normalize_audio_roles(env_map["SOFLE_AUDIO_ROLES"])

    if cli_devices is not None:
        parsed_cli_devs = _parse_audio_csv(cli_devices)
        if parsed_cli_devs:
            devices = parsed_cli_devs

    if include_comms and "communications" not in roles:
        roles = roles + ("communications",)

    return AudioSwapConfig(devices=devices, roles=roles)


def match_audio_endpoint(
    target: str,
    endpoints: List[AudioEndpoint],
    exclude_ids: Optional[set] = None,
) -> Optional[AudioEndpoint]:
    """Match a target name/ID against active endpoints by exact > prefix ('Name' or 'Name (...)') > substring."""
    needle = target.strip().casefold()
    if not needle:
        return None
    excluded = exclude_ids or set()

    def score_endpoint(ep: AudioEndpoint) -> Optional[Tuple[int, int]]:
        dev_id = ep.device_id.strip().casefold()
        fname = ep.friendly_name.strip().casefold()
        desc = ep.device_desc.strip().casefold()

        tier: Optional[int] = None
        if needle == dev_id or needle == fname or (desc and needle == desc):
            tier = 0
        elif fname.startswith(needle) and (
            len(fname) == len(needle) or fname[len(needle)] in (" ", "(")
        ):
            tier = 1
        elif needle in fname or (desc and needle in desc):
            tier = 2

        if tier is None:
            return None
        is_excluded = 1 if ep.device_id in excluded else 0
        return (tier, is_excluded)

    best_ep: Optional[AudioEndpoint] = None
    best_score: Optional[Tuple[int, int]] = None
    for ep in endpoints:
        sc = score_endpoint(ep)
        if sc is not None and (best_score is None or sc < best_score):
            best_score = sc
            best_ep = ep
    return best_ep


def choose_next_audio_endpoint(
    endpoints: List[AudioEndpoint],
    current_default_id: Optional[str],
    targets: Tuple[str, ...],
) -> Tuple[Optional[AudioEndpoint], List[str]]:
    """Resolve target devices and return (next_endpoint_to_select, missing_targets)."""
    resolved: List[AudioEndpoint] = []
    seen_ids = set()
    missing_targets: List[str] = []

    for target in targets:
        ep = match_audio_endpoint(target, endpoints, exclude_ids=seen_ids)
        if ep is None:
            missing_targets.append(target)
            continue
        if ep.device_id not in seen_ids:
            resolved.append(ep)
            seen_ids.add(ep.device_id)

    if not resolved:
        return None, missing_targets

    if current_default_id:
        for i, ep in enumerate(resolved):
            if ep.device_id == current_default_id:
                nxt = resolved[(i + 1) % len(resolved)]
                if nxt.device_id == ep.device_id:
                    return None, missing_targets
                return nxt, missing_targets

    return resolved[0], missing_targets


_HEADSET_KEYWORDS = ("headset", "headphone", "earphone", "earbud", "airpod", "buds")
_SPEAKER_KEYWORDS = ("speaker", "monitor", "hdmi", "display", "soundbar")


def classify_audio_device_name(name: str) -> int:
    """Classify an audio endpoint name/target as Headset (1), Speaker (2), or Unknown (0)."""
    low = name.strip().casefold()
    if not low:
        return HOST_AUDIO_UNKNOWN
    if any(kw in low for kw in _HEADSET_KEYWORDS):
        return HOST_AUDIO_HEADSET
    if any(kw in low for kw in _SPEAKER_KEYWORDS):
        return HOST_AUDIO_SPEAKER
    return HOST_AUDIO_UNKNOWN


def detect_audio_output_state(
    endpoints: List[AudioEndpoint],
    current_default_id: Optional[str],
    targets: Tuple[str, ...],
) -> int:
    """Determine HOST_AUDIO_* for the current Windows default playback endpoint."""
    if not current_default_id:
        return HOST_AUDIO_UNKNOWN

    seen_ids = set()
    for idx, target in enumerate(targets):
        ep = match_audio_endpoint(target, endpoints, exclude_ids=seen_ids)
        if ep is None:
            continue
        seen_ids.add(ep.device_id)
        if ep.device_id == current_default_id:
            state = classify_audio_device_name(target)
            if state == HOST_AUDIO_UNKNOWN:
                state = classify_audio_device_name(ep.friendly_name)
            if state == HOST_AUDIO_UNKNOWN and ep.device_desc:
                state = classify_audio_device_name(ep.device_desc)
            if state == HOST_AUDIO_UNKNOWN:
                state = HOST_AUDIO_HEADSET if (idx % 2 == 0) else HOST_AUDIO_SPEAKER
            return state

    for ep in endpoints:
        if ep.device_id == current_default_id:
            state = classify_audio_device_name(ep.friendly_name)
            if state == HOST_AUDIO_UNKNOWN and ep.device_desc:
                state = classify_audio_device_name(ep.device_desc)
            return state

    return HOST_AUDIO_UNKNOWN


_UNSET_BACKEND = object()


class AudioSwapDispatcher:
    """Swaps Windows default playback output endpoints asynchronously without external dependencies."""

    def __init__(
        self,
        config_path: Optional[str] = None,
        cli_devices: Optional[str] = None,
        include_comms: bool = False,
        verbose: bool = False,
        backend: object = _UNSET_BACKEND,
        on_state_change: Optional[Callable[[int], None]] = None,
        max_inflight: int = 2,
    ) -> None:
        self.config_path = config_path
        self.cli_devices = cli_devices
        self.include_comms = include_comms
        self.verbose = verbose
        self.on_state_change = on_state_change
        if backend is _UNSET_BACKEND:
            self._backend = WindowsCoreAudioBackend() if sys.platform == "win32" else None
        else:
            self._backend = backend
        self._swap_lock = threading.Lock()
        self._inflight = threading.Semaphore(max(1, max_inflight))
        self._monitor_started = False

    def _log(self, msg: str) -> None:
        if self.verbose:
            print(f"[sofle-sync] {msg}", flush=True)

    def set_state_callback(self, cb: Optional[Callable[[int], None]]) -> None:
        self.on_state_change = cb

    def list_devices(self) -> Tuple[List[AudioEndpoint], Optional[str], Optional[str]]:
        if self._backend is None:
            raise OSError("Windows audio device switching is only supported when sofle_sync.py runs on Windows")
        endpoints = self._backend.list_active_render_endpoints()  # type: ignore[attr-defined]
        def_console = self._backend.get_default_render_endpoint_id(role=0)  # type: ignore[attr-defined]
        def_comms = self._backend.get_default_render_endpoint_id(role=2)  # type: ignore[attr-defined]
        return endpoints, def_console, def_comms

    def detect_current_state(self) -> int:
        if self._backend is None:
            return HOST_AUDIO_UNKNOWN
        cfg = load_audio_config(
            config_path=self.config_path,
            cli_devices=self.cli_devices,
            include_comms=self.include_comms,
        )
        role_ints = tuple(AUDIO_ROLE_MAP[r] for r in cfg.roles if r in AUDIO_ROLE_MAP) or (0, 1)
        with self._swap_lock:
            try:
                endpoints = self._backend.list_active_render_endpoints()  # type: ignore[attr-defined]
                cur_default = self._backend.get_default_render_endpoint_id(role=role_ints[0])  # type: ignore[attr-defined]
                if not cur_default and role_ints[0] != 1:
                    cur_default = self._backend.get_default_render_endpoint_id(role=1)  # type: ignore[attr-defined]
                return detect_audio_output_state(endpoints, cur_default, cfg.devices)
            except OSError:
                return HOST_AUDIO_UNKNOWN

    def swap_audio_output(self, side: str = "cli") -> bool:
        if self._backend is None:
            print(
                "[sofle-sync] Windows audio output swap skipped: host is not running on Windows",
                file=sys.stderr,
                flush=True,
            )
            return False

        cfg = load_audio_config(
            config_path=self.config_path,
            cli_devices=self.cli_devices,
            include_comms=self.include_comms,
        )
        role_ints = tuple(AUDIO_ROLE_MAP[r] for r in cfg.roles if r in AUDIO_ROLE_MAP) or (0, 1)

        new_state: Optional[int] = None
        with self._swap_lock:
            try:
                endpoints = self._backend.list_active_render_endpoints()  # type: ignore[attr-defined]
                cur_default = self._backend.get_default_render_endpoint_id(role=role_ints[0])  # type: ignore[attr-defined]
                if not cur_default and role_ints[0] != 1:
                    cur_default = self._backend.get_default_render_endpoint_id(role=1)  # type: ignore[attr-defined]
                target, missing = choose_next_audio_endpoint(endpoints, cur_default, cfg.devices)
                if target is None:
                    avail = ", ".join(ep.friendly_name for ep in endpoints) or "(none)"
                    miss_s = f"Missing: {', '.join(missing)}; " if missing else ""
                    print(
                        f"[sofle-sync] Audio swap target unavailable ({miss_s}active outputs: {avail})",
                        file=sys.stderr,
                        flush=True,
                    )
                    return False

                self._backend.set_default_endpoint(target.device_id, role_ints)  # type: ignore[attr-defined]
                self._log(f"Swapped Windows audio output ({side}) -> {target.friendly_name}")
                new_state = detect_audio_output_state(endpoints, target.device_id, cfg.devices)
            except OSError as exc:
                print(f"[sofle-sync] Windows audio swap failed ({side}): {exc}", file=sys.stderr, flush=True)
                return False

        if new_state is not None:
            cb = self.on_state_change
            if cb is not None:
                cb(new_state)
        return True

    def start_state_monitor(self, interval_s: float = AUDIO_POLL_INTERVAL_S) -> None:
        if self._backend is None or self.on_state_change is None or self._monitor_started:
            return
        self._monitor_started = True
        poll_s = max(0.5, float(interval_s))

        def monitor_loop() -> None:
            last_state: Optional[int] = None
            while True:
                try:
                    cur_state = self.detect_current_state()
                    if last_state is None or cur_state != last_state:
                        last_state = cur_state
                        cb = self.on_state_change
                        if cb is not None:
                            cb(cur_state)
                except Exception as exc:
                    self._log(f"Audio monitor loop error: {exc}")
                time.sleep(poll_s)

        t = threading.Thread(target=monitor_loop, name="sofle-audio-mon", daemon=True)
        t.start()

    def _run_swap_worker(self, side: str) -> None:
        try:
            self.swap_audio_output(side=side)
        finally:
            self._inflight.release()

    def dispatch_async(self, side: str, seq: int) -> None:
        if not self._inflight.acquire(blocking=False):
            print(
                f"[sofle-sync] Dropping audio swap from {side} (seq={seq}): swap already queued",
                file=sys.stderr,
                flush=True,
            )
            return
        t = threading.Thread(
            target=self._run_swap_worker,
            args=(side,),
            name=f"sofle-audio-{seq}",
            daemon=True,
        )
        t.start()


class HostActionRouter:
    """Dispatches Raw HID host actions cleanly by action ID."""

    def __init__(
        self,
        ha_dispatcher: HomeAssistantActionDispatcher,
        audio_dispatcher: AudioSwapDispatcher,
        verbose: bool = False,
    ) -> None:
        self.ha_dispatcher = ha_dispatcher
        self.audio_dispatcher = audio_dispatcher
        self.verbose = verbose

    def dispatch_async(self, side: str, action_id: int, seq: int) -> None:
        if action_id == HOST_ACTION_HA_HEX_TOGGLE:
            self.ha_dispatcher.dispatch_async(side, action_id, seq)
        elif action_id == HOST_ACTION_AUDIO_SWAP:
            self.audio_dispatcher.dispatch_async(side, seq)
        elif self.verbose:
            print(f"[sofle-sync] Ignoring unknown host action id={action_id} from {side} (seq={seq})", flush=True)


def is_qmk_raw_hid_descriptor(desc: bytes) -> bool:
    """
    Parse short items of a USB HID report descriptor to detect the top-level
    QMK Raw HID collection: Usage Page 0xFF60, Usage 0x61.
    """
    usage_page: Optional[int] = None
    first_usage: Optional[int] = None
    i = 0
    n = len(desc)
    while i < n:
        prefix = desc[i]
        i += 1
        if prefix == 0xFE:  # Long item
            if i >= n:
                break
            data_len = desc[i]
            i += 2 + data_len
            continue

        size_code = prefix & 0x03
        size = 4 if size_code == 3 else size_code
        if i + size > n:
            break
        val = int.from_bytes(desc[i : i + size], "little")
        i += size

        b_type = (prefix >> 2) & 0x03
        b_tag = (prefix >> 4) & 0x0F

        if b_type == 1 and b_tag == 0x00:  # Global: Usage Page
            if usage_page is None:
                usage_page = val
        elif b_type == 2 and b_tag == 0x00:  # Local: Usage
            if first_usage is None:
                first_usage = val
        elif b_type == 0 and b_tag == 0x0A:  # Main: Collection
            if usage_page == RAW_USAGE_PAGE and first_usage == RAW_USAGE:
                return True
            break
    return usage_page == RAW_USAGE_PAGE and first_usage == RAW_USAGE


class BridgeCoordinator:
    """Tracks Left/Right device sessions, pairing handshake, and packet routing."""

    def __init__(
        self,
        verbose: bool = False,
        action_handler: Optional[Callable[[str, int, int], None]] = None,
    ) -> None:
        self.verbose = verbose
        self.action_handler = action_handler
        self.lock = threading.RLock()
        self.devices: Dict[str, object] = {}
        self.sides: Dict[str, Optional[str]] = {}  # key -> "left" | "right" | None
        self.roles: Dict[str, Optional[str]] = {"left": None, "right": None}
        self.paired = False
        self.last_ping = 0.0
        self.host_audio_state: int = HOST_AUDIO_UNKNOWN

    def _log(self, msg: str) -> None:
        if self.verbose:
            print(f"[sofle-sync] {msg}", flush=True)

    def _send(self, key: str, payload: bytes) -> bool:
        dev = self.devices.get(key)
        if dev is None:
            return False
        try:
            dev.write_report(payload)  # type: ignore[attr-defined]
            return True
        except OSError as exc:
            self._log(f"Write failed on {key}: {exc}")
            self.unregister_device(key)
            return False

    def register_device(self, key: str, dev: object) -> bool:
        with self.lock:
            self.devices[key] = dev
            self.sides[key] = None
            self._log(f"Connected {key}, probing side...")
            if not self._send(key, build_ping_packet(peer_connected=False, request_state=True)):
                return False
            if self.host_audio_state != HOST_AUDIO_UNKNOWN:
                if not self._send(key, build_host_status_packet(self.host_audio_state)):
                    return False
            return key in self.devices

    def set_host_audio_state(self, state: int, force: bool = False) -> None:
        if state not in (HOST_AUDIO_UNKNOWN, HOST_AUDIO_HEADSET, HOST_AUDIO_SPEAKER):
            state = HOST_AUDIO_UNKNOWN
        with self.lock:
            if not force and state == self.host_audio_state:
                return
            self.host_audio_state = state
            pkt = build_host_status_packet(state)
            for key in list(self.devices.keys()):
                self._send(key, pkt)

    def unregister_device(self, key: str) -> None:
        with self.lock:
            in_roles = any(r == key for r in self.roles.values())
            if key not in self.devices and key not in self.sides and not in_roles:
                return
            dev = self.devices.pop(key, None)
            side = self.sides.pop(key, None)
            if dev is not None:
                try:
                    dev.close()  # type: ignore[attr-defined]
                except Exception:
                    pass
            was_paired = self.paired
            if side in ("left", "right") and self.roles.get(side) == key:
                self.roles[side] = None
            for r_side in ("left", "right"):
                if self.roles.get(r_side) == key:
                    self.roles[r_side] = None
            self.paired = self.roles["left"] is not None and self.roles["right"] is not None
            self._log(f"Disconnected {key} (side={side})")
            if was_paired and not self.paired:
                other = self.roles["right"] or self.roles["left"]
                if other is not None:
                    self._send(other, build_ping_packet(peer_connected=False))

    def handle_packet(self, src_key: str, raw: bytes) -> None:
        payload = normalize_raw_report(raw)
        if payload is None:
            return
        action = parse_host_action(payload)
        if action is not None:
            handler: Optional[Callable[[str, int, int], None]] = None
            with self.lock:
                if src_key not in self.devices:
                    return
                handler = self.action_handler
            if handler is not None:
                handler(action[0], action[1], action[2])
            return
        side = parse_half_state(payload)
        if side is None:
            return  # Not our sync command (e.g. VIA packet)

        other_side = "right" if side == "left" else "left"
        with self.lock:
            if src_key not in self.devices:
                return
            self.sides[src_key] = side
            old_key = self.roles[side]
            if old_key != src_key:
                if old_key is not None and old_key in self.sides:
                    self.sides[old_key] = None
                self.roles[side] = src_key
                self._log(f"Identified {src_key} as {side.upper()}")

            now_paired = self.roles["left"] is not None and self.roles["right"] is not None
            newly_paired = now_paired and not self.paired
            self.paired = now_paired

            peer_key = self.roles[other_side]
            if peer_key is not None and peer_key != src_key:
                self._send(peer_key, payload)

            if newly_paired and self.paired:
                right_key = self.roles["right"]
                left_key = self.roles["left"]
                if right_key is not None and left_key is not None:
                    self._log("Both halves paired! Syncing initial RGB (Right -> Left) and layer/mod states.")
                    self._send(
                        right_key,
                        build_ping_packet(peer_connected=True, request_state=True, request_rgb_authoritative=True),
                    )
                    if self.paired and self.roles["left"] == left_key:
                        self._send(
                            left_key,
                            build_ping_packet(peer_connected=True, request_state=True, request_rgb_authoritative=False),
                        )

    def tick_pings(self, now: Optional[float] = None) -> None:
        if now is None:
            now = time.monotonic()
        with self.lock:
            if now - self.last_ping < PING_INTERVAL_S:
                return
            self.last_ping = now
            for key in list(self.devices.keys()):
                side = self.sides.get(key)
                self._send(
                    key,
                    build_ping_packet(
                        peer_connected=self.paired,
                        request_state=(side is None),
                        request_rgb_authoritative=False,
                    ),
                )


# ---------------------------------------------------------------------------
# Linux /dev/hidraw backend (pure Python stdlib, zero dependencies)
# ---------------------------------------------------------------------------


@dataclass
class LinuxHidrawDevice:
    key: str
    dev_path: str
    fd: int

    def write_report(self, payload: bytes) -> None:
        frame = b"\x00" + payload
        written = os.write(self.fd, frame)
        if written != len(frame):
            raise OSError(f"Short write on {self.dev_path}: {written}")

    def read_report(self) -> Optional[bytes]:
        data = os.read(self.fd, RAW_EPSIZE + 1)
        if not data:
            raise EOFError(f"EOF on {self.dev_path}")
        return data

    def read_report_timeout(self, timeout_ms: int = 250) -> Optional[bytes]:
        if self.fd < 0:
            raise OSError(f"Device closed: {self.dev_path}")
        timeout_s = max(0.0, timeout_ms / 1000.0)
        readable, _, _ = select.select([self.fd], [], [], timeout_s)
        if not readable:
            return None
        return self.read_report()

    def close(self) -> None:
        if self.fd >= 0:
            try:
                os.close(self.fd)
            finally:
                self.fd = -1


def parse_uevent_vid_pid(uevent_text: str) -> Optional[Tuple[int, int]]:
    for line in uevent_text.splitlines():
        if line.startswith("HID_ID="):
            parts = line.split("=", 1)[1].split(":")
            if len(parts) == 3:
                try:
                    return int(parts[1], 16), int(parts[2], 16)
                except ValueError:
                    return None
    return None


def discover_linux_hidraw_paths(sysfs_glob: str = "/sys/class/hidraw/hidraw*") -> List[str]:
    paths: List[str] = []
    for sys_dir in sorted(glob.glob(sysfs_glob)):
        name = os.path.basename(sys_dir)
        uevent_path = os.path.join(sys_dir, "device", "uevent")
        rdesc_path = os.path.join(sys_dir, "device", "report_descriptor")
        try:
            with open(uevent_path, "r", encoding="utf-8", errors="replace") as f:
                vp = parse_uevent_vid_pid(f.read())
            if vp != (VID, PID):
                continue
            with open(rdesc_path, "rb") as f:
                rdesc = f.read()
            if is_qmk_raw_hid_descriptor(rdesc):
                paths.append(f"/dev/{name}")
        except OSError:
            continue
    return paths


def _prune_probe_backoff(
    fail_counts: Dict[str, int],
    retry_after: Dict[str, float],
    active_keys: set,
) -> None:
    """Drop per-path probe backoff state once a device disappears."""
    for stale in list(fail_counts.keys()):
        if stale not in active_keys:
            fail_counts.pop(stale, None)
            retry_after.pop(stale, None)


def _record_probe_failure(
    fail_counts: Dict[str, int],
    retry_after: Dict[str, float],
    key: str,
    now: float,
) -> None:
    """Apply bounded linear backoff when opening or probing a device fails."""
    count = fail_counts.get(key, 0) + 1
    fail_counts[key] = count
    retry_after[key] = now + min(5.0, 2.0 * count)


def _clear_probe_backoff(fail_counts: Dict[str, int], retry_after: Dict[str, float], key: str) -> None:
    fail_counts.pop(key, None)
    retry_after.pop(key, None)


def run_linux_hidraw(coord: BridgeCoordinator) -> None:
    open_devs: Dict[str, LinuxHidrawDevice] = {}
    fail_counts: Dict[str, int] = {}
    retry_after: Dict[str, float] = {}
    last_scan = 0.0

    def rescan(now: float) -> None:
        nonlocal last_scan
        last_scan = now
        found = set(discover_linux_hidraw_paths())
        _prune_probe_backoff(fail_counts, retry_after, found)
        for path in sorted(found):
            if path in open_devs or now < retry_after.get(path, 0.0):
                continue
            try:
                fd = os.open(path, os.O_RDWR | os.O_NONBLOCK)
                dev = LinuxHidrawDevice(key=path, dev_path=path, fd=fd)
            except OSError as exc:
                _record_probe_failure(fail_counts, retry_after, path, now)
                coord._log(f"Cannot open {path}: {exc}")
                continue
            open_devs[path] = dev
            if not coord.register_device(path, dev):
                open_devs.pop(path, None)
                dev.close()
                _record_probe_failure(fail_counts, retry_after, path, now)
                continue
            _clear_probe_backoff(fail_counts, retry_after, path)

    while True:
        now = time.monotonic()
        if len(open_devs) < 2 or (now - last_scan >= RESCAN_INTERVAL_S):
            rescan(now)

        coord.tick_pings(now)

        fd_map = {dev.fd: dev for dev in open_devs.values() if dev.fd >= 0}
        if fd_map:
            readable, _, _ = select.select(list(fd_map.keys()), [], [], 0.05)
            for fd in readable:
                dev = fd_map.get(fd)
                if dev is None:
                    continue
                while True:
                    try:
                        pkt = dev.read_report()
                        if pkt:
                            coord.handle_packet(dev.key, pkt)
                    except BlockingIOError:
                        break
                    except (OSError, EOFError):
                        open_devs.pop(dev.key, None)
                        coord.unregister_device(dev.key)
                        break
        else:
            time.sleep(0.05)

        # Clean up any devices closed during write errors in unregister_device
        for key in list(open_devs.keys()):
            if key not in coord.devices:
                open_devs.pop(key, None)


# ---------------------------------------------------------------------------
# Native Windows HID backend (pure Python stdlib via ctypes setupapi/hid/kernel32)
# ---------------------------------------------------------------------------

DIGCF_PRESENT = 0x02
DIGCF_DEVICEINTERFACE = 0x10
GENERIC_READ = 0x80000000
GENERIC_WRITE = 0x40000000
FILE_SHARE_READ = 0x01
FILE_SHARE_WRITE = 0x02
OPEN_EXISTING = 3
FILE_FLAG_OVERLAPPED = 0x40000000
INVALID_HANDLE_VALUE = ctypes.c_void_p(-1).value
ERROR_IO_PENDING = 997
WAIT_OBJECT_0 = 0
WAIT_TIMEOUT = 258
HIDP_STATUS_SUCCESS = 0x00110000


class Win32GUID(ctypes.Structure):
    _fields_ = [
        ("Data1", ctypes.c_uint32),
        ("Data2", ctypes.c_uint16),
        ("Data3", ctypes.c_uint16),
        ("Data4", ctypes.c_ubyte * 8),
    ]


def make_win32_guid(guid_str: str) -> Win32GUID:
    """Convert a standard GUID string into a Win32GUID ctypes struct."""
    u = uuid.UUID(guid_str)
    return Win32GUID(u.time_low, u.time_mid, u.time_hi_version, (ctypes.c_ubyte * 8)(*u.bytes[8:16]))


class Win32PROPERTYKEY(ctypes.Structure):
    _fields_ = [
        ("fmtid", Win32GUID),
        ("pid", ctypes.c_uint32),
    ]


class Win32PROPVARIANT(ctypes.Structure):
    _fields_ = [
        ("vt", ctypes.c_uint16),
        ("wReserved1", ctypes.c_uint16),
        ("wReserved2", ctypes.c_uint16),
        ("wReserved3", ctypes.c_uint16),
        ("pwszVal", ctypes.c_void_p),
        ("_pad", ctypes.c_void_p),
    ]


CLSID_MMDeviceEnumerator = "{BCDE0395-E52F-467C-8E3D-C4579291692E}"
IID_IMMDeviceEnumerator = "{A95664D2-9614-4F35-A746-DE8DB63617E6}"
CLSID_CPolicyConfigClient = "{870AF99C-171D-4F9E-AF0D-E63DF40C2BC9}"
IID_IPolicyConfig = "{F8679F50-850A-41CF-9C72-430F290290C8}"
CLSID_CPolicyConfigVistaClient = "{294935CE-F637-4E7C-A41B-AB255460B862}"
IID_IPolicyConfigVista = "{568B9108-44BF-40B4-9006-86AFE5B5A620}"
FMTID_DeviceProperties = "{A45C254E-DF1C-4EFD-8020-67D146A850E0}"
PID_DEVICE_FRIENDLY_NAME = 14
PID_DEVICE_DESC = 2

EFLOW_RENDER = 0
DEVICE_STATE_ACTIVE = 0x00000001
STGM_READ = 0
VT_LPWSTR = 31
CLSCTX_ALL = 0x17
COINIT_MULTITHREADED = 0x0
COINIT_APARTMENTTHREADED = 0x2
COM_S_OK = 0
COM_S_FALSE = 1
COM_RPC_E_CHANGED_MODE = -2147417850


class WindowsCoreAudioBackend:
    """Pure-ctypes Windows Core Audio COM endpoint enumerator and switcher."""

    @staticmethod
    def _load_ole32():
        ole32 = ctypes.WinDLL("ole32")  # type: ignore[attr-defined]
        ole32.CoInitializeEx.argtypes = [ctypes.c_void_p, ctypes.c_uint32]
        ole32.CoInitializeEx.restype = ctypes.c_int32
        ole32.CoUninitialize.argtypes = []
        ole32.CoUninitialize.restype = None
        ole32.CoCreateInstance.argtypes = [
            ctypes.POINTER(Win32GUID),
            ctypes.c_void_p,
            ctypes.c_uint32,
            ctypes.POINTER(Win32GUID),
            ctypes.POINTER(ctypes.c_void_p),
        ]
        ole32.CoCreateInstance.restype = ctypes.c_int32
        ole32.CoTaskMemFree.argtypes = [ctypes.c_void_p]
        ole32.CoTaskMemFree.restype = None
        ole32.PropVariantClear.argtypes = [ctypes.POINTER(Win32PROPVARIANT)]
        ole32.PropVariantClear.restype = ctypes.c_int32
        return ole32

    @staticmethod
    def _com_call(iface_ptr: ctypes.c_void_p, slot: int, proto, *args):
        vtable = ctypes.cast(iface_ptr, ctypes.POINTER(ctypes.POINTER(ctypes.c_void_p))).contents
        fn = proto(vtable[slot])
        return fn(iface_ptr, *args)

    @classmethod
    def _com_release(cls, iface_ptr: Optional[ctypes.c_void_p]) -> None:
        if iface_ptr and iface_ptr.value:
            release_proto = ctypes.WINFUNCTYPE(ctypes.c_uint32, ctypes.c_void_p)  # type: ignore[attr-defined]
            cls._com_call(iface_ptr, 2, release_proto)

    def _with_com(self, fn):
        ole32 = self._load_ole32()
        hr = ole32.CoInitializeEx(None, COINIT_MULTITHREADED)
        should_uninit = hr in (COM_S_OK, COM_S_FALSE)
        if hr < 0 and hr != COM_RPC_E_CHANGED_MODE:
            raise OSError(f"CoInitializeEx failed with HRESULT 0x{hr & 0xFFFFFFFF:08X}")
        try:
            return fn(ole32)
        finally:
            if should_uninit:
                ole32.CoUninitialize()

    @staticmethod
    def _co_create(ole32, clsid_str: str, iid_str: str) -> ctypes.c_void_p:
        clsid = make_win32_guid(clsid_str)
        iid = make_win32_guid(iid_str)
        ptr = ctypes.c_void_p()
        hr = ole32.CoCreateInstance(ctypes.byref(clsid), None, CLSCTX_ALL, ctypes.byref(iid), ctypes.byref(ptr))
        if hr < 0 or not ptr.value:
            raise OSError(f"CoCreateInstance({clsid_str}) failed with HRESULT 0x{hr & 0xFFFFFFFF:08X}")
        return ptr

    @classmethod
    def _read_device_id(cls, ole32, p_dev: ctypes.c_void_p) -> Optional[str]:
        get_id_proto = ctypes.WINFUNCTYPE(  # type: ignore[attr-defined]
            ctypes.c_int32, ctypes.c_void_p, ctypes.POINTER(ctypes.c_void_p)
        )
        id_ptr = ctypes.c_void_p()
        try:
            hr = cls._com_call(p_dev, 5, get_id_proto, ctypes.byref(id_ptr))
            if hr < 0 or not id_ptr.value:
                return None
            return ctypes.wstring_at(id_ptr.value)
        finally:
            if id_ptr.value:
                ole32.CoTaskMemFree(id_ptr.value)

    @classmethod
    def _read_prop_string(cls, ole32, p_store: ctypes.c_void_p, pkey: Win32PROPERTYKEY) -> str:
        get_val_proto = ctypes.WINFUNCTYPE(  # type: ignore[attr-defined]
            ctypes.c_int32, ctypes.c_void_p, ctypes.POINTER(Win32PROPERTYKEY), ctypes.POINTER(Win32PROPVARIANT)
        )
        pv = Win32PROPVARIANT()
        ctypes.memset(ctypes.byref(pv), 0, ctypes.sizeof(pv))
        try:
            hr = cls._com_call(p_store, 5, get_val_proto, ctypes.byref(pkey), ctypes.byref(pv))
            if hr >= 0 and pv.vt == VT_LPWSTR and pv.pwszVal:
                return ctypes.wstring_at(pv.pwszVal)
            return ""
        finally:
            ole32.PropVariantClear(ctypes.byref(pv))

    def list_active_render_endpoints(self) -> List[AudioEndpoint]:
        def work(ole32) -> List[AudioEndpoint]:
            enum_endpoints_proto = ctypes.WINFUNCTYPE(  # type: ignore[attr-defined]
                ctypes.c_int32, ctypes.c_void_p, ctypes.c_uint32, ctypes.c_uint32, ctypes.POINTER(ctypes.c_void_p)
            )
            get_count_proto = ctypes.WINFUNCTYPE(  # type: ignore[attr-defined]
                ctypes.c_int32, ctypes.c_void_p, ctypes.POINTER(ctypes.c_uint32)
            )
            item_proto = ctypes.WINFUNCTYPE(  # type: ignore[attr-defined]
                ctypes.c_int32, ctypes.c_void_p, ctypes.c_uint32, ctypes.POINTER(ctypes.c_void_p)
            )
            open_props_proto = ctypes.WINFUNCTYPE(  # type: ignore[attr-defined]
                ctypes.c_int32, ctypes.c_void_p, ctypes.c_uint32, ctypes.POINTER(ctypes.c_void_p)
            )

            pkey_friendly = Win32PROPERTYKEY(make_win32_guid(FMTID_DeviceProperties), PID_DEVICE_FRIENDLY_NAME)
            pkey_desc = Win32PROPERTYKEY(make_win32_guid(FMTID_DeviceProperties), PID_DEVICE_DESC)

            results: List[AudioEndpoint] = []
            p_enum = self._co_create(ole32, CLSID_MMDeviceEnumerator, IID_IMMDeviceEnumerator)
            p_col = ctypes.c_void_p()
            try:
                hr = self._com_call(
                    p_enum, 3, enum_endpoints_proto, EFLOW_RENDER, DEVICE_STATE_ACTIVE, ctypes.byref(p_col)
                )
                if hr < 0 or not p_col.value:
                    raise OSError(f"EnumAudioEndpoints failed with HRESULT 0x{hr & 0xFFFFFFFF:08X}")
                count = ctypes.c_uint32(0)
                hr = self._com_call(p_col, 3, get_count_proto, ctypes.byref(count))
                if hr < 0:
                    raise OSError(f"IMMDeviceCollection::GetCount failed with HRESULT 0x{hr & 0xFFFFFFFF:08X}")

                for idx in range(count.value):
                    p_dev = ctypes.c_void_p()
                    p_store = ctypes.c_void_p()
                    try:
                        hr = self._com_call(p_col, 4, item_proto, idx, ctypes.byref(p_dev))
                        if hr < 0 or not p_dev.value:
                            continue
                        dev_id = self._read_device_id(ole32, p_dev)
                        if not dev_id:
                            continue
                        fname = ""
                        desc = ""
                        hr = self._com_call(p_dev, 4, open_props_proto, STGM_READ, ctypes.byref(p_store))
                        if hr >= 0 and p_store.value:
                            fname = self._read_prop_string(ole32, p_store, pkey_friendly)
                            desc = self._read_prop_string(ole32, p_store, pkey_desc)
                        results.append(
                            AudioEndpoint(
                                device_id=dev_id,
                                friendly_name=fname or desc or dev_id,
                                device_desc=desc,
                            )
                        )
                    finally:
                        self._com_release(p_store)
                        self._com_release(p_dev)
            finally:
                self._com_release(p_col)
                self._com_release(p_enum)
            return results

        return self._with_com(work)

    def get_default_render_endpoint_id(self, role: int = 0) -> Optional[str]:
        def work(ole32) -> Optional[str]:
            get_def_proto = ctypes.WINFUNCTYPE(  # type: ignore[attr-defined]
                ctypes.c_int32, ctypes.c_void_p, ctypes.c_uint32, ctypes.c_uint32, ctypes.POINTER(ctypes.c_void_p)
            )
            p_enum = self._co_create(ole32, CLSID_MMDeviceEnumerator, IID_IMMDeviceEnumerator)
            p_dev = ctypes.c_void_p()
            try:
                hr = self._com_call(p_enum, 4, get_def_proto, EFLOW_RENDER, int(role), ctypes.byref(p_dev))
                if hr < 0 or not p_dev.value:
                    return None
                return self._read_device_id(ole32, p_dev)
            finally:
                self._com_release(p_dev)
                self._com_release(p_enum)

        return self._with_com(work)

    def set_default_endpoint(self, device_id: str, roles: Tuple[int, ...]) -> None:
        def work(ole32) -> None:
            set_def_proto = ctypes.WINFUNCTYPE(  # type: ignore[attr-defined]
                ctypes.c_int32, ctypes.c_void_p, ctypes.c_wchar_p, ctypes.c_uint32
            )
            slot = 13
            try:
                p_cfg = self._co_create(ole32, CLSID_CPolicyConfigClient, IID_IPolicyConfig)
            except OSError:
                p_cfg = self._co_create(ole32, CLSID_CPolicyConfigVistaClient, IID_IPolicyConfigVista)
                slot = 12
            try:
                for role in roles:
                    hr = self._com_call(p_cfg, slot, set_def_proto, device_id, int(role))
                    if hr < 0:
                        raise OSError(f"SetDefaultEndpoint(role={role}) failed with HRESULT 0x{hr & 0xFFFFFFFF:08X}")
            finally:
                self._com_release(p_cfg)

        self._with_com(work)


class SP_DEVICE_INTERFACE_DATA(ctypes.Structure):
    _pack_ = 8 if ctypes.sizeof(ctypes.c_void_p) == 8 else 1
    _fields_ = [
        ("cbSize", ctypes.c_uint32),
        ("InterfaceClassGuid", Win32GUID),
        ("Flags", ctypes.c_uint32),
        ("Reserved", ctypes.c_size_t),
    ]


class HIDD_ATTRIBUTES(ctypes.Structure):
    _fields_ = [
        ("Size", ctypes.c_uint32),
        ("VendorID", ctypes.c_uint16),
        ("ProductID", ctypes.c_uint16),
        ("VersionNumber", ctypes.c_uint16),
    ]


class HIDP_CAPS(ctypes.Structure):
    _fields_ = [
        ("Usage", ctypes.c_uint16),
        ("UsagePage", ctypes.c_uint16),
        ("InputReportByteLength", ctypes.c_uint16),
        ("OutputReportByteLength", ctypes.c_uint16),
        ("FeatureReportByteLength", ctypes.c_uint16),
        ("Reserved", ctypes.c_uint16 * 17),
        ("NumberLinkCollectionNodes", ctypes.c_uint16),
        ("NumberInputButtonCaps", ctypes.c_uint16),
        ("NumberInputValueCaps", ctypes.c_uint16),
        ("NumberInputDataIndices", ctypes.c_uint16),
        ("NumberOutputButtonCaps", ctypes.c_uint16),
        ("NumberOutputValueCaps", ctypes.c_uint16),
        ("NumberOutputDataIndices", ctypes.c_uint16),
        ("NumberFeatureButtonCaps", ctypes.c_uint16),
        ("NumberFeatureValueCaps", ctypes.c_uint16),
        ("NumberFeatureDataIndices", ctypes.c_uint16),
    ]


class Win32OVERLAPPED(ctypes.Structure):
    _fields_ = [
        ("Internal", ctypes.c_size_t),
        ("InternalHigh", ctypes.c_size_t),
        ("Offset", ctypes.c_uint32),
        ("OffsetHigh", ctypes.c_uint32),
        ("hEvent", ctypes.c_void_p),
    ]


def _default_last_error() -> int:
    return 0


@dataclass
class Win32HidApis:
    HidD_GetHidGuid: object
    HidD_GetAttributes: object
    HidD_GetPreparsedData: object
    HidD_FreePreparsedData: object
    HidD_SetNumInputBuffers: object
    HidP_GetCaps: object
    SetupDiGetClassDevsW: object
    SetupDiEnumDeviceInterfaces: object
    SetupDiGetDeviceInterfaceDetailW: object
    SetupDiDestroyDeviceInfoList: object
    CreateFileW: object
    ReadFile: object
    WriteFile: object
    CloseHandle: object
    CreateEventW: object
    ResetEvent: object
    WaitForSingleObject: object
    GetOverlappedResult: object
    CancelIoEx: object
    get_last_error: Callable[[], int] = _default_last_error


def _load_win32_apis() -> Win32HidApis:
    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)  # type: ignore[attr-defined]
    setupapi = ctypes.WinDLL("setupapi", use_last_error=True)  # type: ignore[attr-defined]
    hid = ctypes.WinDLL("hid", use_last_error=True)  # type: ignore[attr-defined]

    hid.HidD_GetHidGuid.argtypes = [ctypes.POINTER(Win32GUID)]
    hid.HidD_GetHidGuid.restype = None

    hid.HidD_GetAttributes.argtypes = [ctypes.c_void_p, ctypes.POINTER(HIDD_ATTRIBUTES)]
    hid.HidD_GetAttributes.restype = ctypes.c_ubyte

    hid.HidD_GetPreparsedData.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_void_p)]
    hid.HidD_GetPreparsedData.restype = ctypes.c_ubyte

    hid.HidD_FreePreparsedData.argtypes = [ctypes.c_void_p]
    hid.HidD_FreePreparsedData.restype = ctypes.c_ubyte

    hid.HidD_SetNumInputBuffers.argtypes = [ctypes.c_void_p, ctypes.c_uint32]
    hid.HidD_SetNumInputBuffers.restype = ctypes.c_ubyte

    hid.HidP_GetCaps.argtypes = [ctypes.c_void_p, ctypes.POINTER(HIDP_CAPS)]
    hid.HidP_GetCaps.restype = ctypes.c_int32

    setupapi.SetupDiGetClassDevsW.argtypes = [
        ctypes.POINTER(Win32GUID),
        ctypes.c_wchar_p,
        ctypes.c_void_p,
        ctypes.c_uint32,
    ]
    setupapi.SetupDiGetClassDevsW.restype = ctypes.c_void_p

    setupapi.SetupDiEnumDeviceInterfaces.argtypes = [
        ctypes.c_void_p,
        ctypes.c_void_p,
        ctypes.POINTER(Win32GUID),
        ctypes.c_uint32,
        ctypes.POINTER(SP_DEVICE_INTERFACE_DATA),
    ]
    setupapi.SetupDiEnumDeviceInterfaces.restype = ctypes.c_int

    setupapi.SetupDiGetDeviceInterfaceDetailW.argtypes = [
        ctypes.c_void_p,
        ctypes.POINTER(SP_DEVICE_INTERFACE_DATA),
        ctypes.c_void_p,
        ctypes.c_uint32,
        ctypes.POINTER(ctypes.c_uint32),
        ctypes.c_void_p,
    ]
    setupapi.SetupDiGetDeviceInterfaceDetailW.restype = ctypes.c_int

    setupapi.SetupDiDestroyDeviceInfoList.argtypes = [ctypes.c_void_p]
    setupapi.SetupDiDestroyDeviceInfoList.restype = ctypes.c_int

    kernel32.CreateFileW.argtypes = [
        ctypes.c_wchar_p,
        ctypes.c_uint32,
        ctypes.c_uint32,
        ctypes.c_void_p,
        ctypes.c_uint32,
        ctypes.c_uint32,
        ctypes.c_void_p,
    ]
    kernel32.CreateFileW.restype = ctypes.c_void_p

    kernel32.ReadFile.argtypes = [
        ctypes.c_void_p,
        ctypes.c_void_p,
        ctypes.c_uint32,
        ctypes.POINTER(ctypes.c_uint32),
        ctypes.POINTER(Win32OVERLAPPED),
    ]
    kernel32.ReadFile.restype = ctypes.c_int

    kernel32.WriteFile.argtypes = [
        ctypes.c_void_p,
        ctypes.c_void_p,
        ctypes.c_uint32,
        ctypes.POINTER(ctypes.c_uint32),
        ctypes.POINTER(Win32OVERLAPPED),
    ]
    kernel32.WriteFile.restype = ctypes.c_int

    kernel32.CloseHandle.argtypes = [ctypes.c_void_p]
    kernel32.CloseHandle.restype = ctypes.c_int

    kernel32.CreateEventW.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_int, ctypes.c_wchar_p]
    kernel32.CreateEventW.restype = ctypes.c_void_p

    kernel32.ResetEvent.argtypes = [ctypes.c_void_p]
    kernel32.ResetEvent.restype = ctypes.c_int

    kernel32.WaitForSingleObject.argtypes = [ctypes.c_void_p, ctypes.c_uint32]
    kernel32.WaitForSingleObject.restype = ctypes.c_uint32

    kernel32.GetOverlappedResult.argtypes = [
        ctypes.c_void_p,
        ctypes.POINTER(Win32OVERLAPPED),
        ctypes.POINTER(ctypes.c_uint32),
        ctypes.c_int,
    ]
    kernel32.GetOverlappedResult.restype = ctypes.c_int

    kernel32.CancelIoEx.argtypes = [ctypes.c_void_p, ctypes.POINTER(Win32OVERLAPPED)]
    kernel32.CancelIoEx.restype = ctypes.c_int

    return Win32HidApis(
        HidD_GetHidGuid=hid.HidD_GetHidGuid,
        HidD_GetAttributes=hid.HidD_GetAttributes,
        HidD_GetPreparsedData=hid.HidD_GetPreparsedData,
        HidD_FreePreparsedData=hid.HidD_FreePreparsedData,
        HidD_SetNumInputBuffers=hid.HidD_SetNumInputBuffers,
        HidP_GetCaps=hid.HidP_GetCaps,
        SetupDiGetClassDevsW=setupapi.SetupDiGetClassDevsW,
        SetupDiEnumDeviceInterfaces=setupapi.SetupDiEnumDeviceInterfaces,
        SetupDiGetDeviceInterfaceDetailW=setupapi.SetupDiGetDeviceInterfaceDetailW,
        SetupDiDestroyDeviceInfoList=setupapi.SetupDiDestroyDeviceInfoList,
        CreateFileW=kernel32.CreateFileW,
        ReadFile=kernel32.ReadFile,
        WriteFile=kernel32.WriteFile,
        CloseHandle=kernel32.CloseHandle,
        CreateEventW=kernel32.CreateEventW,
        ResetEvent=kernel32.ResetEvent,
        WaitForSingleObject=kernel32.WaitForSingleObject,
        GetOverlappedResult=kernel32.GetOverlappedResult,
        CancelIoEx=kernel32.CancelIoEx,
        get_last_error=ctypes.get_last_error,
    )


def _probe_win32_device(apis: Win32HidApis, path: str) -> Optional[Tuple[int, int]]:
    """Return (input_len, output_len) if path is our Sofle Pico Raw HID endpoint, else None."""
    handle = apis.CreateFileW(
        path,
        0,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        None,
        OPEN_EXISTING,
        0,
        None,
    )
    if not handle or handle == INVALID_HANDLE_VALUE:
        return None
    try:
        attrs = HIDD_ATTRIBUTES(Size=ctypes.sizeof(HIDD_ATTRIBUTES))
        if not apis.HidD_GetAttributes(handle, ctypes.byref(attrs)):
            return None
        if attrs.VendorID != VID or attrs.ProductID != PID:
            return None

        preparsed = ctypes.c_void_p()
        if not apis.HidD_GetPreparsedData(handle, ctypes.byref(preparsed)) or not preparsed.value:
            return None
        try:
            caps = HIDP_CAPS()
            if apis.HidP_GetCaps(preparsed, ctypes.byref(caps)) != HIDP_STATUS_SUCCESS:
                return None
            if caps.UsagePage != RAW_USAGE_PAGE or caps.Usage != RAW_USAGE:
                return None
            in_len = max(caps.InputReportByteLength, RAW_EPSIZE + 1)
            out_len = max(caps.OutputReportByteLength, RAW_EPSIZE + 1)
            return (in_len, out_len)
        finally:
            apis.HidD_FreePreparsedData(preparsed)
    finally:
        apis.CloseHandle(handle)


def discover_win32_hid_devices(apis: Win32HidApis) -> List[Tuple[str, int, int]]:
    """Enumerate all connected Sofle Pico Raw HID interfaces on Windows."""
    hid_guid = Win32GUID()
    apis.HidD_GetHidGuid(ctypes.byref(hid_guid))
    dev_info = apis.SetupDiGetClassDevsW(
        ctypes.byref(hid_guid),
        None,
        None,
        DIGCF_PRESENT | DIGCF_DEVICEINTERFACE,
    )
    if not dev_info or dev_info == INVALID_HANDLE_VALUE:
        return []

    results: List[Tuple[str, int, int]] = []
    detail_cb = 8 if ctypes.sizeof(ctypes.c_void_p) == 8 else 6
    try:
        index = 0
        while True:
            if_data = SP_DEVICE_INTERFACE_DATA(cbSize=ctypes.sizeof(SP_DEVICE_INTERFACE_DATA))
            if not apis.SetupDiEnumDeviceInterfaces(dev_info, None, ctypes.byref(hid_guid), index, ctypes.byref(if_data)):
                break
            index += 1

            req_size = ctypes.c_uint32(0)
            apis.SetupDiGetDeviceInterfaceDetailW(dev_info, ctypes.byref(if_data), None, 0, ctypes.byref(req_size), None)
            if req_size.value <= 4:
                continue

            buf = ctypes.create_string_buffer(req_size.value)
            struct.pack_into("<I", buf, 0, detail_cb)
            if not apis.SetupDiGetDeviceInterfaceDetailW(
                dev_info, ctypes.byref(if_data), buf, req_size.value, ctypes.byref(req_size), None
            ):
                continue

            device_path = bytes(buf[4 : req_size.value]).decode("utf-16-le").split("\x00", 1)[0]
            lens = _probe_win32_device(apis, device_path)
            if lens is not None:
                results.append((device_path, lens[0], lens[1]))
    finally:
        apis.SetupDiDestroyDeviceInfoList(dev_info)

    return sorted(results, key=lambda item: item[0])


class Win32HidDevice:
    """Thread-safe overlapped I/O handle wrapper for a Windows HID endpoint."""

    def __init__(self, apis: Win32HidApis, path: str, input_len: int, output_len: int) -> None:
        self._apis = apis
        self.key = path
        self.input_len = input_len
        self.output_len = output_len
        self._closed = False
        self._handles_closed = False
        self._io_refs = 0
        self._handle = None
        self._read_ev = None
        self._write_ev = None
        self._close_lock = threading.Lock()
        self._write_lock = threading.Lock()

        handle = apis.CreateFileW(
            path,
            GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            None,
            OPEN_EXISTING,
            FILE_FLAG_OVERLAPPED,
            None,
        )
        if not handle or handle == INVALID_HANDLE_VALUE:
            err = apis.get_last_error()
            raise OSError(err, f"CreateFileW failed on {path}: error {err}")

        self._handle = handle
        apis.HidD_SetNumInputBuffers(self._handle, 64)
        self._read_ev = apis.CreateEventW(None, 1, 0, None)
        self._write_ev = apis.CreateEventW(None, 1, 0, None)
        if not self._read_ev or not self._write_ev:
            err = apis.get_last_error()
            self.close()
            raise OSError(err, f"CreateEventW failed on {path}: error {err}")

    def _acquire_io(self) -> Tuple[object, object, object]:
        with self._close_lock:
            if self._closed or not self._handle or self._handle == INVALID_HANDLE_VALUE:
                raise OSError(f"Device closed: {self.key}")
            self._io_refs += 1
            return self._handle, self._read_ev, self._write_ev

    def _pop_handles_if_ready_locked(self) -> Tuple[object, ...]:
        if not (self._closed and self._io_refs == 0 and not self._handles_closed):
            return ()
        self._handles_closed = True
        handles = tuple(
            h
            for h in (self._handle, self._read_ev, self._write_ev)
            if h and h != INVALID_HANDLE_VALUE
        )
        self._handle = None
        self._read_ev = None
        self._write_ev = None
        return handles

    def _close_handles(self, handles: Tuple[object, ...]) -> None:
        for h in handles:
            if h and h != INVALID_HANDLE_VALUE:
                self._apis.CloseHandle(h)

    def _release_io(self) -> None:
        with self._close_lock:
            if self._io_refs > 0:
                self._io_refs -= 1
            handles = self._pop_handles_if_ready_locked()
        self._close_handles(handles)

    def write_report(self, payload: bytes) -> None:
        frame = (b"\x00" + payload).ljust(self.output_len, b"\x00")[: self.output_len]
        tx_buf = ctypes.create_string_buffer(frame, len(frame))
        written = ctypes.c_uint32(0)

        handle, _, write_ev = self._acquire_io()
        try:
            with self._write_lock:
                if self._closed:
                    raise OSError(f"Device closed: {self.key}")
                self._apis.ResetEvent(write_ev)
                ov = Win32OVERLAPPED(hEvent=write_ev)
                ok = self._apis.WriteFile(handle, tx_buf, len(frame), ctypes.byref(written), ctypes.byref(ov))
                if not ok:
                    err = self._apis.get_last_error()
                    if err != ERROR_IO_PENDING:
                        raise OSError(err, f"WriteFile failed on {self.key}: error {err}")
                    wres = self._apis.WaitForSingleObject(write_ev, 500)
                    if wres == WAIT_OBJECT_0:
                        if not self._apis.GetOverlappedResult(handle, ctypes.byref(ov), ctypes.byref(written), 0):
                            err = self._apis.get_last_error()
                            raise OSError(err, f"GetOverlappedResult(write) failed on {self.key}: error {err}")
                    else:
                        # CancelIoEx is async: wait for kernel IRP completion before releasing ov/tx_buf.
                        self._apis.CancelIoEx(handle, ctypes.byref(ov))
                        completed = bool(
                            self._apis.GetOverlappedResult(handle, ctypes.byref(ov), ctypes.byref(written), 1)
                        )
                        if not (wres == WAIT_TIMEOUT and completed and written.value >= len(frame)):
                            raise OSError(f"WriteFile wait failed on {self.key}: {wres}")
        finally:
            self._release_io()

    def read_report(self, timeout_ms: int = 250) -> Optional[bytes]:
        handle, read_ev, _ = self._acquire_io()
        try:
            rx_buf = ctypes.create_string_buffer(self.input_len)
            bytes_read = ctypes.c_uint32(0)
            self._apis.ResetEvent(read_ev)
            ov = Win32OVERLAPPED(hEvent=read_ev)
            ok = self._apis.ReadFile(handle, rx_buf, self.input_len, ctypes.byref(bytes_read), ctypes.byref(ov))
            if not ok:
                err = self._apis.get_last_error()
                if err != ERROR_IO_PENDING:
                    raise OSError(err, f"ReadFile failed on {self.key}: error {err}")
                wres = self._apis.WaitForSingleObject(read_ev, max(0, int(timeout_ms)))
                if wres == WAIT_OBJECT_0:
                    if not self._apis.GetOverlappedResult(handle, ctypes.byref(ov), ctypes.byref(bytes_read), 0):
                        err = self._apis.get_last_error()
                        raise OSError(err, f"GetOverlappedResult(read) failed on {self.key}: error {err}")
                elif wres == WAIT_TIMEOUT:
                    self._apis.CancelIoEx(handle, ctypes.byref(ov))
                    bytes_read.value = 0
                    if not self._apis.GetOverlappedResult(handle, ctypes.byref(ov), ctypes.byref(bytes_read), 1):
                        return None
                    if bytes_read.value > 0:
                        return bytes(rx_buf[: bytes_read.value])
                    return None
                else:
                    self._apis.CancelIoEx(handle, ctypes.byref(ov))
                    self._apis.GetOverlappedResult(handle, ctypes.byref(ov), ctypes.byref(bytes_read), 1)
                    raise OSError(f"ReadFile wait failed on {self.key}: {wres}")
            if bytes_read.value > 0:
                return bytes(rx_buf[: bytes_read.value])
            return None
        finally:
            self._release_io()

    def read_report_timeout(self, timeout_ms: int = 250) -> Optional[bytes]:
        return self.read_report(timeout_ms=timeout_ms)

    def read_loop(self, stop_ev: threading.Event, on_packet: Callable[[str, bytes], None]) -> None:
        rx_buf = ctypes.create_string_buffer(self.input_len)
        bytes_read = ctypes.c_uint32(0)

        try:
            handle, read_ev, _ = self._acquire_io()
        except OSError:
            return

        try:
            while not stop_ev.is_set() and not self._closed:
                self._apis.ResetEvent(read_ev)
                ov = Win32OVERLAPPED(hEvent=read_ev)
                bytes_read.value = 0
                ok = self._apis.ReadFile(handle, rx_buf, self.input_len, ctypes.byref(bytes_read), ctypes.byref(ov))
                if not ok:
                    err = self._apis.get_last_error()
                    if err != ERROR_IO_PENDING:
                        break
                    while True:
                        wres = self._apis.WaitForSingleObject(read_ev, 250)
                        if stop_ev.is_set() or self._closed:
                            self._apis.CancelIoEx(handle, ctypes.byref(ov))
                            self._apis.GetOverlappedResult(handle, ctypes.byref(ov), ctypes.byref(bytes_read), 1)
                            return
                        if wres == WAIT_OBJECT_0:
                            if not self._apis.GetOverlappedResult(handle, ctypes.byref(ov), ctypes.byref(bytes_read), 0):
                                return
                            break
                        if wres == WAIT_TIMEOUT:
                            continue
                        self._apis.CancelIoEx(handle, ctypes.byref(ov))
                        self._apis.GetOverlappedResult(handle, ctypes.byref(ov), ctypes.byref(bytes_read), 1)
                        return

                if bytes_read.value > 0:
                    on_packet(self.key, bytes(rx_buf[: bytes_read.value]))
        finally:
            self._release_io()

    def close(self) -> None:
        handle = None
        handles: Tuple[object, ...] = ()
        with self._close_lock:
            if self._closed:
                return
            self._closed = True
            if self._handle and self._handle != INVALID_HANDLE_VALUE:
                handle = self._handle
                self._io_refs += 1
            else:
                handles = self._pop_handles_if_ready_locked()
        if handle is not None:
            try:
                self._apis.CancelIoEx(handle, None)
            finally:
                self._release_io()
        else:
            self._close_handles(handles)


def run_win32_hid(coord: BridgeCoordinator) -> None:
    apis = _load_win32_apis()
    threads: Dict[str, Tuple[threading.Event, Win32HidDevice]] = {}
    fail_counts: Dict[str, int] = {}
    retry_after: Dict[str, float] = {}

    def reader_worker(key: str, stop_ev: threading.Event, dev: Win32HidDevice) -> None:
        try:
            dev.read_loop(stop_ev, coord.handle_packet)
        finally:
            threads.pop(key, None)
            coord.unregister_device(key)

    last_scan = 0.0
    while True:
        now = time.monotonic()
        if len(threads) < 2 or (now - last_scan >= RESCAN_INTERVAL_S):
            last_scan = now
            discovered = discover_win32_hid_devices(apis)
            _prune_probe_backoff(fail_counts, retry_after, {path for path, _, _ in discovered})
            for path, in_len, out_len in discovered:
                if path in threads or now < retry_after.get(path, 0.0):
                    continue
                try:
                    dev = Win32HidDevice(apis, path, in_len, out_len)
                except OSError as exc:
                    _record_probe_failure(fail_counts, retry_after, path, now)
                    coord._log(f"Cannot open {path}: {exc}")
                    continue

                stop_ev = threading.Event()
                threads[path] = (stop_ev, dev)
                if not coord.register_device(path, dev):
                    threads.pop(path, None)
                    dev.close()
                    _record_probe_failure(fail_counts, retry_after, path, now)
                    continue

                _clear_probe_backoff(fail_counts, retry_after, path)
                t = threading.Thread(
                    target=reader_worker,
                    args=(path, stop_ev, dev),
                    name=f"sofle-win32-{len(threads)}",
                    daemon=True,
                )
                t.start()

        coord.tick_pings(now)
        time.sleep(0.1)


# ---------------------------------------------------------------------------
# Optional hidapi backend (macOS fallback, or explicit --backend hidapi)
# ---------------------------------------------------------------------------


def _import_hid_module():
    try:
        import hid  # type: ignore
        return hid
    except ImportError as exc:
        raise RuntimeError(
            "The 'hid' package (hidapi) is required for --backend hidapi. "
            "Install it with: pip install hidapi"
        ) from exc


class HidapiDeviceWrapper:
    def __init__(self, hid_mod, raw_path) -> None:
        self._legacy_api = hasattr(hid_mod, "device")
        if self._legacy_api:
            self._dev = hid_mod.device()
            self._dev.open_path(raw_path)
            self._dev.set_nonblocking(0)
        else:
            self._dev = hid_mod.Device(path=raw_path)

    def write_report(self, payload: bytes) -> None:
        frame = b"\x00" + payload
        if self._legacy_api:
            res = self._dev.write(list(frame))
            if res < 0:
                raise OSError("hidapi write failed")
        else:
            self._dev.write(frame)

    def read_report(self, timeout_ms: int = 250) -> bytes:
        if self._legacy_api:
            data = self._dev.read(RAW_EPSIZE + 1, timeout_ms=timeout_ms)
            return bytes(data) if data else b""
        data = self._dev.read(RAW_EPSIZE + 1, timeout=timeout_ms)
        return bytes(data) if data else b""

    def read_report_timeout(self, timeout_ms: int = 250) -> Optional[bytes]:
        data = self.read_report(timeout_ms=timeout_ms)
        return data if data else None

    def close(self) -> None:
        try:
            self._dev.close()
        except Exception:
            pass


def _hidapi_path_key(raw_path) -> str:
    if isinstance(raw_path, (bytes, bytearray)):
        return raw_path.decode("utf-8", errors="replace")
    return str(raw_path)


def discover_hidapi_paths(hid_mod) -> List[Tuple[str, object]]:
    results: List[Tuple[str, object]] = []
    for info in hid_mod.enumerate(VID, PID):
        if info.get("usage_page") == RAW_USAGE_PAGE and info.get("usage") == RAW_USAGE:
            raw_path = info["path"]
            results.append((_hidapi_path_key(raw_path), raw_path))
    return sorted(results, key=lambda item: item[0])


def run_hidapi(coord: BridgeCoordinator) -> None:
    hid_mod = _import_hid_module()
    threads: Dict[str, Tuple[threading.Event, HidapiDeviceWrapper]] = {}
    fail_counts: Dict[str, int] = {}
    retry_after: Dict[str, float] = {}

    def reader_loop(key: str, stop_ev: threading.Event, dev: HidapiDeviceWrapper) -> None:
        try:
            while not stop_ev.is_set():
                try:
                    pkt = dev.read_report(timeout_ms=250)
                except OSError:
                    break
                if pkt:
                    coord.handle_packet(key, pkt)
        finally:
            threads.pop(key, None)
            coord.unregister_device(key)

    last_scan = 0.0
    while True:
        now = time.monotonic()
        if len(threads) < 2 or (now - last_scan >= RESCAN_INTERVAL_S):
            last_scan = now
            discovered = discover_hidapi_paths(hid_mod)
            _prune_probe_backoff(fail_counts, retry_after, {key for key, _ in discovered})
            for key, raw_path in discovered:
                if key in threads or now < retry_after.get(key, 0.0):
                    continue
                try:
                    dev = HidapiDeviceWrapper(hid_mod, raw_path)
                except OSError as exc:
                    _record_probe_failure(fail_counts, retry_after, key, now)
                    coord._log(f"Cannot open {key}: {exc}")
                    continue

                stop_ev = threading.Event()
                threads[key] = (stop_ev, dev)
                if not coord.register_device(key, dev):
                    threads.pop(key, None)
                    dev.close()
                    _record_probe_failure(fail_counts, retry_after, key, now)
                    continue

                _clear_probe_backoff(fail_counts, retry_after, key)
                t = threading.Thread(
                    target=reader_loop,
                    args=(key, stop_ev, dev),
                    name=f"sofle-read-{len(threads)}",
                    daemon=True,
                )
                t.start()

        coord.tick_pings(now)
        time.sleep(0.1)


def resolve_backend(backend: str, platform: Optional[str] = None) -> str:
    if backend != "auto":
        return backend
    plat = sys.platform if platform is None else platform
    if plat.startswith("linux"):
        return "hidraw"
    if plat == "win32":
        return "win32"
    return "hidapi"


def main(argv: Optional[List[str]] = None) -> int:
    parser = argparse.ArgumentParser(description="Sofle Pico Dual-USB Raw HID sync service")
    parser.add_argument(
        "--backend",
        choices=("auto", "hidraw", "win32", "hidapi"),
        default="auto",
        help="HID backend (default: hidraw on Linux, win32 on Windows, hidapi on macOS)",
    )
    parser.add_argument("--verbose", "-v", action="store_true", help="Log pairing and hotplug events")
    parser.add_argument("--ha-config", default=None, help="Path to ha_config.json override")
    parser.add_argument("--ha-url", default=None, help="Home Assistant base URL override")
    parser.add_argument("--ha-webhook-id", default=None, help="Home Assistant webhook ID override")
    parser.add_argument(
        "--test-hex-toggle",
        action="store_true",
        help="Send a one-shot Home Assistant hex lights toggle request and exit",
    )
    parser.add_argument("--audio-config", default=None, help="Path to audio_config.json override")
    parser.add_argument(
        "--audio-devices",
        default=None,
        help="Comma-separated Windows playback device names to swap (e.g. 'Wireless Headset,Speakers')",
    )
    parser.add_argument(
        "--audio-include-comms",
        action="store_true",
        help="Also switch the Windows Default Communications audio device when swapping",
    )
    parser.add_argument(
        "--list-audio-devices",
        action="store_true",
        help="List active Windows playback endpoints and exit",
    )
    parser.add_argument(
        "--test-audio-swap",
        action="store_true",
        help="Swap between the configured Windows audio output devices once and exit",
    )
    args = parser.parse_args(argv)

    ha_dispatcher = HomeAssistantActionDispatcher(
        config_path=args.ha_config,
        cli_url=args.ha_url,
        cli_webhook_id=args.ha_webhook_id,
        verbose=args.verbose,
    )

    if args.test_hex_toggle:
        ok = ha_dispatcher.trigger_hex_toggle(side="cli")
        return 0 if ok else 1

    audio_dispatcher = AudioSwapDispatcher(
        config_path=args.audio_config,
        cli_devices=args.audio_devices,
        include_comms=args.audio_include_comms,
        verbose=args.verbose,
    )

    if args.list_audio_devices:
        try:
            endpoints, def_console, def_comms = audio_dispatcher.list_devices()
        except OSError as exc:
            print(f"[sofle-sync] {exc}", file=sys.stderr, flush=True)
            return 1
        for ep in endpoints:
            tags: List[str] = []
            if ep.device_id == def_console:
                tags.append("DEFAULT")
            if ep.device_id == def_comms:
                tags.append("COMMS")
            tag_str = f" [{', '.join(tags)}]" if tags else ""
            print(f"{ep.friendly_name}{tag_str}", flush=True)
        return 0

    if args.test_audio_swap:
        ok = audio_dispatcher.swap_audio_output(side="cli")
        return 0 if ok else 1

    router = HostActionRouter(
        ha_dispatcher=ha_dispatcher,
        audio_dispatcher=audio_dispatcher,
        verbose=args.verbose,
    )

    backend = resolve_backend(args.backend)

    coord = BridgeCoordinator(verbose=args.verbose, action_handler=router.dispatch_async)
    audio_dispatcher.set_state_callback(coord.set_host_audio_state)
    audio_dispatcher.start_state_monitor()
    if args.verbose:
        print(f"[sofle-sync] Starting on {sys.platform} using '{backend}' backend", flush=True)

    try:
        if backend == "hidraw":
            run_linux_hidraw(coord)
        elif backend == "win32":
            run_win32_hid(coord)
        else:
            run_hidapi(coord)
    except KeyboardInterrupt:
        return 0
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
