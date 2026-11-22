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
import os
import select
import struct
import sys
import threading
import time
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
BOOTLOADER_MAGIC = b"BOOT"

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

    def __init__(self, verbose: bool = False) -> None:
        self.verbose = verbose
        self.lock = threading.RLock()
        self.devices: Dict[str, object] = {}
        self.sides: Dict[str, Optional[str]] = {}  # key -> "left" | "right" | None
        self.roles: Dict[str, Optional[str]] = {"left": None, "right": None}
        self.paired = False
        self.last_ping = 0.0

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
        except OSError:
            self.unregister_device(key)
            return False

    def register_device(self, key: str, dev: object) -> None:
        with self.lock:
            self.devices[key] = dev
            self.sides[key] = None
            self._log(f"Connected {key}, probing side...")
            self._send(key, build_ping_packet(peer_connected=False, request_state=True))

    def unregister_device(self, key: str) -> None:
        with self.lock:
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

            if newly_paired:
                self._log("Both halves paired! Syncing initial RGB (Right -> Left) and layer/mod states.")
                assert self.roles["right"] is not None and self.roles["left"] is not None
                self._send(
                    self.roles["right"],
                    build_ping_packet(peer_connected=True, request_state=True, request_rgb_authoritative=True),
                )
                self._send(
                    self.roles["left"],
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


def run_linux_hidraw(coord: BridgeCoordinator) -> None:
    open_devs: Dict[str, LinuxHidrawDevice] = {}
    last_scan = 0.0

    def rescan(now: float) -> None:
        nonlocal last_scan
        last_scan = now
        found = set(discover_linux_hidraw_paths())
        for path in sorted(found):
            if path not in open_devs:
                try:
                    fd = os.open(path, os.O_RDWR | os.O_NONBLOCK)
                    dev = LinuxHidrawDevice(key=path, dev_path=path, fd=fd)
                    open_devs[path] = dev
                    coord.register_device(path, dev)
                except OSError as exc:
                    coord._log(f"Cannot open {path}: {exc}")

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

    def write_report(self, payload: bytes) -> None:
        frame = (b"\x00" + payload).ljust(self.output_len, b"\x00")[: self.output_len]
        tx_buf = ctypes.create_string_buffer(frame, len(frame))
        written = ctypes.c_uint32(0)

        with self._write_lock:
            if self._closed or not self._handle:
                raise OSError(f"Device closed: {self.key}")
            self._apis.ResetEvent(self._write_ev)
            ov = Win32OVERLAPPED(hEvent=self._write_ev)
            ok = self._apis.WriteFile(self._handle, tx_buf, len(frame), ctypes.byref(written), ctypes.byref(ov))
            if not ok:
                err = self._apis.get_last_error()
                if err != ERROR_IO_PENDING:
                    raise OSError(err, f"WriteFile failed on {self.key}: error {err}")
                wres = self._apis.WaitForSingleObject(self._write_ev, 500)
                if wres != WAIT_OBJECT_0:
                    self._apis.CancelIoEx(self._handle, ctypes.byref(ov))
                    raise OSError(f"WriteFile wait failed on {self.key}: {wres}")
                if not self._apis.GetOverlappedResult(self._handle, ctypes.byref(ov), ctypes.byref(written), 0):
                    err = self._apis.get_last_error()
                    raise OSError(err, f"GetOverlappedResult(write) failed on {self.key}: error {err}")

    def read_report(self, timeout_ms: int = 250) -> Optional[bytes]:
        if self._closed or not self._handle:
            raise OSError(f"Device closed: {self.key}")
        rx_buf = ctypes.create_string_buffer(self.input_len)
        bytes_read = ctypes.c_uint32(0)
        self._apis.ResetEvent(self._read_ev)
        ov = Win32OVERLAPPED(hEvent=self._read_ev)
        ok = self._apis.ReadFile(self._handle, rx_buf, self.input_len, ctypes.byref(bytes_read), ctypes.byref(ov))
        if not ok:
            err = self._apis.get_last_error()
            if err != ERROR_IO_PENDING:
                raise OSError(err, f"ReadFile failed on {self.key}: error {err}")
            wres = self._apis.WaitForSingleObject(self._read_ev, max(0, int(timeout_ms)))
            if wres == WAIT_OBJECT_0:
                if not self._apis.GetOverlappedResult(self._handle, ctypes.byref(ov), ctypes.byref(bytes_read), 0):
                    err = self._apis.get_last_error()
                    raise OSError(err, f"GetOverlappedResult(read) failed on {self.key}: error {err}")
            elif wres == WAIT_TIMEOUT:
                self._apis.CancelIoEx(self._handle, ctypes.byref(ov))
                bytes_read.value = 0
                if not self._apis.GetOverlappedResult(self._handle, ctypes.byref(ov), ctypes.byref(bytes_read), 1):
                    return None
                if bytes_read.value > 0:
                    return bytes(rx_buf[: bytes_read.value])
                return None
            else:
                self._apis.CancelIoEx(self._handle, ctypes.byref(ov))
                self._apis.GetOverlappedResult(self._handle, ctypes.byref(ov), ctypes.byref(bytes_read), 1)
                raise OSError(f"ReadFile wait failed on {self.key}: {wres}")
        if bytes_read.value > 0:
            return bytes(rx_buf[: bytes_read.value])
        return None

    def read_report_timeout(self, timeout_ms: int = 250) -> Optional[bytes]:
        return self.read_report(timeout_ms=timeout_ms)

    def read_loop(self, stop_ev: threading.Event, on_packet: Callable[[str, bytes], None]) -> None:
        rx_buf = ctypes.create_string_buffer(self.input_len)
        bytes_read = ctypes.c_uint32(0)

        while not stop_ev.is_set() and not self._closed:
            self._apis.ResetEvent(self._read_ev)
            ov = Win32OVERLAPPED(hEvent=self._read_ev)
            bytes_read.value = 0
            ok = self._apis.ReadFile(self._handle, rx_buf, self.input_len, ctypes.byref(bytes_read), ctypes.byref(ov))
            if not ok:
                err = self._apis.get_last_error()
                if err != ERROR_IO_PENDING:
                    break
                while True:
                    wres = self._apis.WaitForSingleObject(self._read_ev, 250)
                    if stop_ev.is_set() or self._closed:
                        self._apis.CancelIoEx(self._handle, ctypes.byref(ov))
                        self._apis.GetOverlappedResult(self._handle, ctypes.byref(ov), ctypes.byref(bytes_read), 1)
                        return
                    if wres == WAIT_OBJECT_0:
                        if not self._apis.GetOverlappedResult(self._handle, ctypes.byref(ov), ctypes.byref(bytes_read), 0):
                            return
                        break
                    if wres != WAIT_TIMEOUT:
                        return

            if bytes_read.value > 0:
                on_packet(self.key, bytes(rx_buf[: bytes_read.value]))

    def close(self) -> None:
        with self._close_lock:
            if self._closed:
                return
            self._closed = True
            handle = self._handle
            read_ev = getattr(self, "_read_ev", None)
            write_ev = getattr(self, "_write_ev", None)
            self._handle = None
            self._read_ev = None
            self._write_ev = None
            if handle and handle != INVALID_HANDLE_VALUE:
                try:
                    self._apis.CancelIoEx(handle, None)
                finally:
                    self._apis.CloseHandle(handle)
            if read_ev:
                self._apis.CloseHandle(read_ev)
            if write_ev:
                self._apis.CloseHandle(write_ev)


def run_win32_hid(coord: BridgeCoordinator) -> None:
    apis = _load_win32_apis()
    threads: Dict[str, Tuple[threading.Event, Win32HidDevice]] = {}

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
            for path, in_len, out_len in discover_win32_hid_devices(apis):
                if path not in threads:
                    try:
                        dev = Win32HidDevice(apis, path, in_len, out_len)
                        stop_ev = threading.Event()
                        threads[path] = (stop_ev, dev)
                        coord.register_device(path, dev)
                        t = threading.Thread(
                            target=reader_worker,
                            args=(path, stop_ev, dev),
                            name=f"sofle-win32-{len(threads)}",
                            daemon=True,
                        )
                        t.start()
                    except OSError as exc:
                        threads.pop(path, None)
                        coord._log(f"Cannot open {path}: {exc}")

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
            for key, raw_path in discover_hidapi_paths(hid_mod):
                if key not in threads:
                    try:
                        dev = HidapiDeviceWrapper(hid_mod, raw_path)
                        stop_ev = threading.Event()
                        threads[key] = (stop_ev, dev)
                        coord.register_device(key, dev)
                        t = threading.Thread(
                            target=reader_loop,
                            args=(key, stop_ev, dev),
                            name=f"sofle-read-{len(threads)}",
                            daemon=True,
                        )
                        t.start()
                    except OSError as exc:
                        threads.pop(key, None)
                        coord._log(f"Cannot open {key}: {exc}")

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
    args = parser.parse_args(argv)

    backend = resolve_backend(args.backend)

    coord = BridgeCoordinator(verbose=args.verbose)
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
