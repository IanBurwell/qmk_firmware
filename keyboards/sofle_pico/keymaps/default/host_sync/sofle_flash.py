#!/usr/bin/env python3
"""
Cross-platform (Windows PowerShell, WSL, Linux) single-command dual-board UF2 flasher
for the Sofle Pico split keyboard.

Flashes Left then Right sequentially over USB without manual BOOT button presses
once firmware with RAW_MSG_IDENTIFY (0x03) and RAW_MSG_BOOTLOADER (0x04) is running,
with automatic one-time manual bootstrap fallback for older firmware.
"""

from __future__ import annotations

import argparse
import ctypes
import os
import shlex
import shutil
import struct
import subprocess
import sys
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Callable, Dict, List, Optional, Sequence, Set, Tuple

_THIS_DIR = os.path.dirname(os.path.abspath(__file__))
if _THIS_DIR not in sys.path:
    sys.path.insert(0, _THIS_DIR)

import sofle_sync  # noqa: E402

UF2_MAGIC_START0 = 0x0A324655
UF2_MAGIC_START1 = 0x9E5D5157
UF2_MAGIC_END = 0x0AB16F30
UF2_FLAG_FAMILY_ID_PRESENT = 0x00002000
RP2040_FAMILY_ID = 0xE48BFF56
UF2_BLOCK_SIZE = 512

SEM_FAILCRITICALERRORS = 0x0001
DRIVE_REMOVABLE = 2

REPO_ROOT = Path(__file__).resolve().parents[5]
DEFAULT_UF2_PATH = REPO_ROOT / "sofle_pico_default.uf2"


@dataclass(frozen=True)
class IdentifyInfo:
    side: str
    trrs_connected: bool
    uptime_ms: int


def validate_uf2_bytes(data: bytes) -> int:
    """Validate UF2 container structure and RP2040 family ID; return block count."""
    if not data:
        raise ValueError("UF2 file is empty.")
    if len(data) % UF2_BLOCK_SIZE != 0:
        raise ValueError(f"UF2 size ({len(data)} bytes) is not a multiple of {UF2_BLOCK_SIZE}.")

    total_blocks = len(data) // UF2_BLOCK_SIZE
    for i in range(total_blocks):
        off = i * UF2_BLOCK_SIZE
        block = data[off : off + UF2_BLOCK_SIZE]
        magic0, magic1, flags, _target_addr, _payload_size, block_no, num_blocks, family_id = struct.unpack_from(
            "<IIIIIIII", block, 0
        )
        (magic_end,) = struct.unpack_from("<I", block, UF2_BLOCK_SIZE - 4)

        if magic0 != UF2_MAGIC_START0 or magic1 != UF2_MAGIC_START1 or magic_end != UF2_MAGIC_END:
            raise ValueError(f"Invalid UF2 block magic in block {i}.")
        if block_no != i or num_blocks != total_blocks:
            raise ValueError(
                f"Corrupt UF2 sequence at block {i}: block_no={block_no}, num_blocks={num_blocks}, expected={total_blocks}."
            )
        if (flags & UF2_FLAG_FAMILY_ID_PRESENT) and family_id != RP2040_FAMILY_ID:
            raise ValueError(
                f"UF2 family ID mismatch in block {i}: 0x{family_id:08X} != 0x{RP2040_FAMILY_ID:08X} (RP2040)."
            )

    return total_blocks


def build_identify_packet(nonce: int) -> bytes:
    """Build a 32-byte RAW_MSG_IDENTIFY probe packet."""
    pkt = bytearray(sofle_sync.RAW_EPSIZE)
    pkt[0] = sofle_sync.CMD_SYNC
    pkt[1:3] = sofle_sync.MAGIC
    pkt[3] = sofle_sync.MSG_IDENTIFY
    struct.pack_into(">H", pkt, 4, nonce & 0xFFFF)
    return bytes(pkt)


def parse_identify_reply(payload: bytes, expected_nonce: int) -> Optional[IdentifyInfo]:
    """Parse RAW_MSG_IDENTIFY response if valid and matching expected_nonce."""
    if len(payload) != sofle_sync.RAW_EPSIZE:
        return None
    if payload[0] != sofle_sync.CMD_SYNC or payload[1:3] != sofle_sync.MAGIC or payload[3] != sofle_sync.MSG_IDENTIFY:
        return None
    nonce = (payload[6] << 8) | payload[7]
    if nonce != (expected_nonce & 0xFFFF):
        return None
    side = "left" if payload[4] != 0 else "right"
    trrs_connected = payload[5] != 0
    uptime_ms = int.from_bytes(payload[8:12], "little")
    return IdentifyInfo(side=side, trrs_connected=trrs_connected, uptime_ms=uptime_ms)


def build_bootloader_packet(side: str, nonce: int) -> bytes:
    """Build a 32-byte RAW_MSG_BOOTLOADER command packet for 'left', 'right', or 'any'."""
    side_map = {"right": 0, "left": 1, "any": 2}
    if side not in side_map:
        raise ValueError(f"Invalid target side: {side!r}")
    pkt = bytearray(sofle_sync.RAW_EPSIZE)
    pkt[0] = sofle_sync.CMD_SYNC
    pkt[1:3] = sofle_sync.MAGIC
    pkt[3] = sofle_sync.MSG_BOOTLOADER
    pkt[4] = side_map[side]
    pkt[5:9] = sofle_sync.BOOTLOADER_MAGIC
    struct.pack_into(">H", pkt, 9, nonce & 0xFFFF)
    return bytes(pkt)


def parse_bootloader_ack(payload: bytes, expected_nonce: int) -> Optional[Tuple[str, bool]]:
    """Return (side, accepted) for a valid matching RAW_MSG_BOOTLOADER ACK, else None."""
    if len(payload) != sofle_sync.RAW_EPSIZE:
        return None
    if payload[0] != sofle_sync.CMD_SYNC or payload[1:3] != sofle_sync.MAGIC or payload[3] != sofle_sync.MSG_BOOTLOADER:
        return None
    nonce = (payload[6] << 8) | payload[7]
    if nonce != (expected_nonce & 0xFFFF):
        return None
    side = "left" if payload[4] != 0 else "right"
    accepted = payload[5] != 0
    return side, accepted


def exchange_report(
    dev: object,
    req_pkt: bytes,
    matcher: Callable[[bytes], Optional[object]],
    timeout_s: float = 0.5,
    clock: Callable[[], float] = time.monotonic,
    sleep: Callable[[float], None] = time.sleep,
):
    """Send a Raw HID packet and wait until matcher() accepts a reply or timeout_s elapses."""
    dev.write_report(req_pkt)  # type: ignore[attr-defined]
    reader = getattr(dev, "read_report_timeout", None) or dev.read_report  # type: ignore[attr-defined]
    deadline = clock() + timeout_s

    while True:
        now = clock()
        remaining_s = deadline - now
        if remaining_s <= 0:
            return None
        slice_ms = max(1, min(200, int(remaining_s * 1000)))
        try:
            raw = reader(slice_ms)
        except (OSError, EOFError):
            return None
        if raw:
            norm = sofle_sync.normalize_raw_report(raw)
            if norm is not None:
                res = matcher(norm)
                if res is not None:
                    return res
        elif clock() <= now:
            sleep(slice_ms / 1000.0)
        if clock() >= deadline:
            return None


def _is_rp2040_uf2_mount(mount_path: str) -> bool:
    info_path = os.path.join(mount_path, "INFO_UF2.TXT")
    try:
        if not os.path.isfile(info_path):
            return False
        with open(info_path, "r", encoding="utf-8", errors="replace") as f:
            text = f.read(4096).upper()
        return ("RPI-RP2" in text) or ("RP2040" in text)
    except OSError:
        return False


# ---------------------------------------------------------------------------
# RPI-RP2 Mass-Storage Discovery & Writing (Windows + Linux)
# ---------------------------------------------------------------------------


@dataclass
class Win32DriveApis:
    SetErrorMode: object
    GetLogicalDrives: object
    GetDriveTypeW: object
    GetVolumeInformationW: object


def _load_win32_drive_apis() -> Win32DriveApis:
    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)  # type: ignore[attr-defined]

    kernel32.SetErrorMode.argtypes = [ctypes.c_uint32]
    kernel32.SetErrorMode.restype = ctypes.c_uint32

    kernel32.GetLogicalDrives.argtypes = []
    kernel32.GetLogicalDrives.restype = ctypes.c_uint32

    kernel32.GetDriveTypeW.argtypes = [ctypes.c_wchar_p]
    kernel32.GetDriveTypeW.restype = ctypes.c_uint32

    kernel32.GetVolumeInformationW.argtypes = [
        ctypes.c_wchar_p,
        ctypes.c_wchar_p,
        ctypes.c_uint32,
        ctypes.POINTER(ctypes.c_uint32),
        ctypes.POINTER(ctypes.c_uint32),
        ctypes.POINTER(ctypes.c_uint32),
        ctypes.c_wchar_p,
        ctypes.c_uint32,
    ]
    kernel32.GetVolumeInformationW.restype = ctypes.c_int

    return Win32DriveApis(
        SetErrorMode=kernel32.SetErrorMode,
        GetLogicalDrives=kernel32.GetLogicalDrives,
        GetDriveTypeW=kernel32.GetDriveTypeW,
        GetVolumeInformationW=kernel32.GetVolumeInformationW,
    )


def find_rp2_mounts_win32(
    apis: Win32DriveApis,
    validator: Callable[[str], bool] = _is_rp2040_uf2_mount,
) -> List[str]:
    """Enumerate mounted RPI-RP2 UF2 removable drives on Windows via kernel32."""
    prev_mode = apis.SetErrorMode(SEM_FAILCRITICALERRORS)
    mounts: List[str] = []
    try:
        mask = apis.GetLogicalDrives()
        for idx in range(26):
            if not (mask & (1 << idx)):
                continue
            root = f"{chr(ord('A') + idx)}:\\"
            if apis.GetDriveTypeW(root) != DRIVE_REMOVABLE:
                continue
            vol_buf = ctypes.create_unicode_buffer(261)
            fs_buf = ctypes.create_unicode_buffer(261)
            ok = apis.GetVolumeInformationW(root, vol_buf, 261, None, None, None, fs_buf, 261)
            if not ok:
                continue
            if validator(root):
                mounts.append(root)
    finally:
        apis.SetErrorMode(prev_mode)
    return mounts


def _unescape_proc_mounts(token: str) -> str:
    return (
        token.replace("\\040", " ")
        .replace("\\011", "\t")
        .replace("\\012", "\n")
        .replace("\\134", "\\")
    )


def _scan_proc_mounts(
    proc_mounts_path: str,
    validator: Callable[[str], bool],
) -> List[str]:
    mounts: List[str] = []
    try:
        with open(proc_mounts_path, "r", encoding="utf-8", errors="replace") as f:
            for line in f:
                parts = line.split()
                if len(parts) < 3:
                    continue
                mountpoint = _unescape_proc_mounts(parts[1])
                fstype = parts[2].lower()
                if fstype in ("vfat", "msdos") and validator(mountpoint):
                    if mountpoint not in mounts:
                        mounts.append(mountpoint)
    except OSError:
        return []
    return sorted(mounts)


def find_rp2_mounts_linux(
    proc_mounts_path: str = "/proc/mounts",
    by_label_path: str = "/dev/disk/by-label/RPI-RP2",
    auto_mount: bool = True,
    validator: Callable[[str], bool] = _is_rp2040_uf2_mount,
    which: Callable[[str], Optional[str]] = shutil.which,
    run_cmd: Callable[..., object] = subprocess.run,
) -> List[str]:
    """Find mounted RPI-RP2 drives on Linux, auto-mounting via udisksctl if unmounted."""
    mounts = _scan_proc_mounts(proc_mounts_path, validator)
    if mounts or not auto_mount:
        return mounts

    if os.path.exists(by_label_path):
        udisksctl = which("udisksctl")
        if udisksctl:
            dev_node = os.path.realpath(by_label_path)
            try:
                run_cmd(
                    [udisksctl, "mount", "-b", dev_node, "--no-user-interaction"],
                    capture_output=True,
                    text=True,
                    timeout=10,
                )
            except (OSError, subprocess.SubprocessError):
                return []
            mounts = _scan_proc_mounts(proc_mounts_path, validator)
    return mounts


def find_rp2_mounts_darwin(validator: Callable[[str], bool] = _is_rp2040_uf2_mount) -> List[str]:
    volumes_dir = "/Volumes"
    if not os.path.isdir(volumes_dir):
        return []
    mounts: List[str] = []
    for entry in sorted(os.listdir(volumes_dir)):
        path = os.path.join(volumes_dir, entry)
        if os.path.isdir(path) and validator(path):
            mounts.append(path)
    return mounts


def write_uf2_to_mount(
    mount_path: str,
    uf2_bytes: bytes,
    filename: str = "sofle_pico_default.uf2",
) -> None:
    """
    Write uf2_bytes sequentially to mount_path/filename.

    Tolerates OSError occurring on the final UF2 block, flush, fsync, or close,
    because RP2040 immediately resets out of USB mass-storage mode upon receiving
    the last block. Re-raises OSError if opening or earlier writes fail.
    """
    dest = os.path.join(mount_path, filename)
    f = open(dest, "wb")
    reached_final = False
    try:
        total = len(uf2_bytes)
        chunk_size = 4096
        offset = 0
        while offset < total:
            end = min(total, offset + chunk_size)
            chunk = uf2_bytes[offset:end]
            if end >= total:
                reached_final = True
                try:
                    f.write(chunk)
                except OSError:
                    return
            else:
                f.write(chunk)
            offset = end

        try:
            f.flush()
            os.fsync(f.fileno())
        except OSError:
            return
    finally:
        exc_active = sys.exc_info()[0] is not None
        try:
            f.close()
        except OSError:
            if not reached_final and not exc_active:
                raise


# ---------------------------------------------------------------------------
# WSL <-> Windows Helpers & Build Execution
# ---------------------------------------------------------------------------


def parse_wsl_unc_path(path: str) -> Optional[Tuple[str, str]]:
    """Parse \\\\wsl.localhost\\<distro>\\... or \\\\wsl$\\<distro>\\... into (distro, linux_path)."""
    norm = path.replace("/", "\\")
    lower = norm.lower()
    for prefix in ("\\\\wsl.localhost\\", "\\\\wsl$\\"):
        if lower.startswith(prefix):
            rest = norm[len(prefix) :]
            parts = rest.split("\\", 1)
            distro = parts[0]
            if not distro:
                return None
            linux_tail = parts[1].replace("\\", "/") if len(parts) > 1 else ""
            linux_path = "/" + linux_tail.lstrip("/")
            return distro, linux_path
    return None


def is_wsl() -> bool:
    if os.environ.get("WSL_DISTRO_NAME"):
        return True
    try:
        with open("/proc/version", "r", encoding="utf-8", errors="replace") as f:
            return "microsoft" in f.read().lower()
    except OSError:
        return False


def build_windows_wsl_compile_cmd(distro: Optional[str], linux_repo_root: str) -> List[str]:
    """Construct the wsl.exe command to compile sofle_pico:default inside WSL."""
    cmd = ["wsl.exe"]
    if distro:
        cmd.extend(["-d", distro])
    inner = f'cd {shlex.quote(linux_repo_root)} && PATH="$HOME/.local/bin:$PATH" qmk compile -kb sofle_pico -km default'
    cmd.extend(["--", "bash", "-lc", inner])
    return cmd


def _find_native_qmk() -> Optional[str]:
    qmk_bin = shutil.which("qmk")
    if qmk_bin:
        return qmk_bin
    candidate = os.path.expanduser("~/.local/bin/qmk")
    if os.path.isfile(candidate) and os.access(candidate, os.X_OK):
        return candidate
    return None


def run_build(repo_root: Path, wsl_distro: Optional[str] = None) -> None:
    """Compile sofle_pico:default via native QMK (Linux/WSL) or via wsl.exe (Windows)."""
    print("[sofle-flash] Building sofle_pico:default firmware...", flush=True)
    if sys.platform == "win32":
        unc = parse_wsl_unc_path(str(repo_root))
        if unc is not None or wsl_distro:
            distro = wsl_distro or (unc[0] if unc is not None else None)
            linux_root = unc[1] if unc is not None else str(repo_root)
            cmd = build_windows_wsl_compile_cmd(distro, linux_root)
            res = subprocess.run(cmd)
        else:
            qmk_bin = shutil.which("qmk") or shutil.which("qmk.exe")
            if qmk_bin:
                res = subprocess.run([qmk_bin, "compile", "-kb", "sofle_pico", "-km", "default"], cwd=str(repo_root))
            else:
                # Fallback to default WSL distro if native qmk isn't on Windows PATH
                cmd = build_windows_wsl_compile_cmd(None, "/home/en/qmk_firmware")
                res = subprocess.run(cmd)
    else:
        qmk_bin = _find_native_qmk()
        if not qmk_bin:
            raise RuntimeError("Could not find 'qmk' CLI (checked PATH and ~/.local/bin/qmk).")
        res = subprocess.run([qmk_bin, "compile", "-kb", "sofle_pico", "-km", "default"], cwd=str(repo_root))

    if res.returncode != 0:
        raise RuntimeError(f"Firmware build failed with exit code {res.returncode}.")


def build_wsl_handoff_args(
    win_script_path: str,
    win_uf2_path: str,
    side: str,
    timeout: float,
    verbose: bool,
) -> List[str]:
    """Build argument list to invoke Windows python.exe for flashing from WSL."""
    cmd = [
        win_script_path,
        "--uf2",
        win_uf2_path,
        "--side",
        side,
        "--timeout",
        str(timeout),
        "--backend",
        "win32",
    ]
    if verbose:
        cmd.append("--verbose")
    return cmd


def maybe_delegate_wsl_to_windows(args: argparse.Namespace, uf2_path: Path) -> Optional[int]:
    """When run inside WSL Linux, delegate USB HID/drive flashing to Windows python.exe."""
    if not (sys.platform.startswith("linux") and is_wsl() and args.backend in ("auto", "win32")):
        return None

    python_exe = shutil.which("python.exe")
    if not python_exe:
        print(
            "[sofle-flash] Error: Inside WSL, Windows python.exe is needed to access USB HID and RPI-RP2 drives, "
            "but python.exe was not found on PATH.",
            file=sys.stderr,
        )
        return 1

    try:
        win_script = subprocess.check_output(["wslpath", "-w", str(Path(__file__).resolve())], text=True).strip()
        win_uf2 = subprocess.check_output(["wslpath", "-w", str(uf2_path.resolve())], text=True).strip()
    except (OSError, subprocess.SubprocessError) as exc:
        print(f"[sofle-flash] Error converting paths via wslpath: {exc}", file=sys.stderr)
        return 1

    cmd = [python_exe] + build_wsl_handoff_args(win_script, win_uf2, args.side, args.timeout, args.verbose)
    res = subprocess.run(cmd)
    return res.returncode


# ---------------------------------------------------------------------------
# Backend Wrapper & Sequential Flash State Machine
# ---------------------------------------------------------------------------


@dataclass
class FlashBackend:
    discover_paths: Callable[[], List[object]]
    open_device: Callable[[object], object]
    find_rp2_mounts: Callable[[], List[str]]
    write_uf2: Callable[[str, bytes, str], None] = write_uf2_to_mount
    sleep: Callable[[float], None] = time.sleep
    clock: Callable[[], float] = time.monotonic
    prompt_manual_bootstrap: Optional[Callable[[str, float], None]] = None


def create_flash_backend(backend_name: str) -> FlashBackend:
    if backend_name == "hidraw":
        return FlashBackend(
            discover_paths=sofle_sync.discover_linux_hidraw_paths,
            open_device=lambda p: sofle_sync.LinuxHidrawDevice(
                key=str(p), dev_path=str(p), fd=os.open(str(p), os.O_RDWR | os.O_NONBLOCK)
            ),
            find_rp2_mounts=find_rp2_mounts_linux,
        )
    if backend_name == "win32":
        hid_apis = sofle_sync._load_win32_apis()
        drive_apis = _load_win32_drive_apis()
        return FlashBackend(
            discover_paths=lambda: sofle_sync.discover_win32_hid_devices(hid_apis),
            open_device=lambda item: sofle_sync.Win32HidDevice(hid_apis, item[0], item[1], item[2]),  # type: ignore[index]
            find_rp2_mounts=lambda: find_rp2_mounts_win32(drive_apis),
        )
    hid_mod = sofle_sync._import_hid_module()
    if sys.platform == "win32":
        drive_apis = _load_win32_drive_apis()
        mount_fn: Callable[[], List[str]] = lambda: find_rp2_mounts_win32(drive_apis)
    elif sys.platform == "darwin":
        mount_fn = find_rp2_mounts_darwin
    else:
        mount_fn = find_rp2_mounts_linux
    return FlashBackend(
        discover_paths=lambda: sofle_sync.discover_hidapi_paths(hid_mod),
        open_device=lambda item: sofle_sync.HidapiDeviceWrapper(hid_mod, item[1]),  # type: ignore[index]
        find_rp2_mounts=mount_fn,
    )


class _NonceGen:
    def __init__(self, start: int = 1) -> None:
        self._val = start & 0xFFFF

    def next(self) -> int:
        self._val = (self._val + 1) & 0xFFFF
        if self._val == 0:
            self._val = 1
        return self._val


def _probe_halves(
    backend: FlashBackend,
    nonce_gen: _NonceGen,
    per_dev_timeout_s: float = 0.45,
) -> Tuple[Dict[str, Tuple[object, IdentifyInfo]], bool, int]:
    identified: Dict[str, Tuple[object, IdentifyInfo]] = {}
    trrs_detected = False
    unresponsive = 0

    for path_info in backend.discover_paths():
        try:
            dev = backend.open_device(path_info)
        except OSError:
            unresponsive += 1
            continue
        try:
            nonce = nonce_gen.next()
            info = exchange_report(
                dev,
                build_identify_packet(nonce),
                lambda pkt, n=nonce: parse_identify_reply(pkt, n),
                timeout_s=per_dev_timeout_s,
                clock=backend.clock,
                sleep=backend.sleep,
            )
            if isinstance(info, IdentifyInfo):
                identified[info.side] = (path_info, info)
                if info.trrs_connected:
                    trrs_detected = True
            else:
                unresponsive += 1
        finally:
            try:
                dev.close()  # type: ignore[attr-defined]
            except Exception:
                pass

    return identified, trrs_detected, unresponsive


def _wait_for_mount_appear(
    backend: FlashBackend,
    excluded: Set[str],
    timeout_s: float,
    poll_s: float = 0.1,
) -> Optional[str]:
    deadline = backend.clock() + timeout_s
    while True:
        mounts = backend.find_rp2_mounts()
        fresh = [m for m in mounts if m not in excluded]
        if len(fresh) == 1:
            return fresh[0]
        if len(fresh) > 1:
            raise RuntimeError(f"Multiple RPI-RP2 drives appeared simultaneously: {fresh}")
        if not excluded and len(mounts) == 1:
            return mounts[0]
        if backend.clock() >= deadline:
            return None
        backend.sleep(poll_s)


def _wait_for_mount_disappear(
    backend: FlashBackend,
    mount_path: str,
    timeout_s: float,
    poll_s: float = 0.1,
) -> bool:
    deadline = backend.clock() + timeout_s
    while True:
        if mount_path not in backend.find_rp2_mounts():
            return True
        if backend.clock() >= deadline:
            return False
        backend.sleep(poll_s)


def _wait_for_side_reenumerate(
    backend: FlashBackend,
    side: str,
    prev_uptime_ms: Optional[int],
    nonce_gen: _NonceGen,
    timeout_s: float,
    poll_s: float = 0.15,
) -> Optional[IdentifyInfo]:
    deadline = backend.clock() + timeout_s
    while True:
        identified, _, _ = _probe_halves(backend, nonce_gen, per_dev_timeout_s=0.3)
        if side in identified:
            _, info = identified[side]
            if prev_uptime_ms is None or info.uptime_ms < prev_uptime_ms or info.uptime_ms < 15000:
                return info
        if backend.clock() >= deadline:
            return None
        backend.sleep(poll_s)


def flash_boards(
    backend: FlashBackend,
    uf2_bytes: bytes,
    uf2_filename: str = "sofle_pico_default.uf2",
    side: str = "both",
    timeout_s: float = 20.0,
    verbose: bool = False,
) -> bool:
    """Validate UF2 bytes and sequentially flash requested Sofle Pico halves."""
    block_count = validate_uf2_bytes(uf2_bytes)
    nonce_gen = _NonceGen()

    def log(msg: str) -> None:
        if verbose:
            print(f"[sofle-flash] {msg}", flush=True)

    log(f"Validated UF2 ({len(uf2_bytes)} bytes, {block_count} blocks, RP2040 OK).")

    wanted_sides: Sequence[str] = ("left", "right") if side == "both" else (side,)
    already_flashed: Set[str] = set()
    flashed_order: List[str] = []

    # 1. Handle any board already sitting in RPI-RP2 bootloader mode
    pre_mounts = backend.find_rp2_mounts()
    if len(pre_mounts) > 1:
        print(
            f"[sofle-flash] Error: Multiple RPI-RP2 drives already mounted ({pre_mounts}). "
            "Leave at most one board in bootloader mode so sides cannot be confused.",
            file=sys.stderr,
        )
        return False

    if len(pre_mounts) == 1:
        mount = pre_mounts[0]
        before_ids, _, _ = _probe_halves(backend, nonce_gen)
        before_sides = set(before_ids.keys())
        print(f"[sofle-flash] Found pre-mounted bootloader drive at {mount}; flashing {uf2_filename}...", flush=True)
        backend.write_uf2(mount, uf2_bytes, uf2_filename)
        if not _wait_for_mount_disappear(backend, mount, timeout_s):
            print(f"[sofle-flash] Error: Bootloader drive {mount} did not unmount after writing UF2.", file=sys.stderr)
            return False

        # Wait briefly for the rebooted half to identify itself over Raw HID
        re_deadline = backend.clock() + min(timeout_s, 8.0)
        detected_pre_side: Optional[str] = None
        while backend.clock() < re_deadline:
            after_ids, _, _ = _probe_halves(backend, nonce_gen, per_dev_timeout_s=0.3)
            new_sides = set(after_ids.keys()) - before_sides
            if len(new_sides) == 1:
                detected_pre_side = next(iter(new_sides))
                break
            backend.sleep(0.15)

        if detected_pre_side is not None:
            already_flashed.add(detected_pre_side)
            flashed_order.append(detected_pre_side)
            print(f"[sofle-flash] [ok] {detected_pre_side.upper()} half flashed and rebooted.", flush=True)
        elif side != "both":
            # Single-side explicit request when pre-mounted board came back
            already_flashed.add(side)
            flashed_order.append(side)
            print(f"[sofle-flash] [ok] {side.upper()} half flashed from bootloader drive.", flush=True)
        else:
            print(
                "[sofle-flash] Warning: Pre-mounted RPI-RP2 half flashed, but could not identify which side it is.",
                flush=True,
            )

    # 2. Discover remaining halves over Raw HID
    identified, trrs_detected, unresponsive = _probe_halves(backend, nonce_gen)
    targets = [s for s in wanted_sides if s not in already_flashed]
    if not targets:
        print(f"[sofle-flash] Done! Flashed: {', '.join(s.upper() for s in flashed_order)}.", flush=True)
        return True

    for target in targets:
        existing_mounts = set(backend.find_rp2_mounts())
        prev_uptime: Optional[int] = None

        if target in identified:
            path_info, info = identified[target]
            prev_uptime = info.uptime_ms
            if info.trrs_connected:
                trrs_detected = True

            print(
                f"[sofle-flash] Rebooting {target.upper()} half into RPI-RP2 bootloader (uptime {prev_uptime} ms)...",
                flush=True,
            )
            try:
                dev = backend.open_device(path_info)
            except OSError as exc:
                print(f"[sofle-flash] Error opening {target.upper()} HID interface: {exc}", file=sys.stderr)
                return False
            try:
                nonce = nonce_gen.next()
                ack = exchange_report(
                    dev,
                    build_bootloader_packet(target, nonce),
                    lambda pkt, n=nonce: parse_bootloader_ack(pkt, n),
                    timeout_s=1.0,
                    clock=backend.clock,
                    sleep=backend.sleep,
                )
            finally:
                try:
                    dev.close()  # type: ignore[attr-defined]
                except Exception:
                    pass

            if ack is None:
                print(f"[sofle-flash] Error: Timed out waiting for {target.upper()} bootloader ACK.", file=sys.stderr)
                return False
            ack_side, accepted = ack
            if not accepted or ack_side != target:
                print(
                    f"[sofle-flash] Error: Bootloader request rejected by {ack_side.upper()} (accepted={accepted}).",
                    file=sys.stderr,
                )
                return False
            log(f"Bootloader ACK received from {target.upper()}; waiting for RPI-RP2 drive...")
            mount = _wait_for_mount_appear(backend, existing_mounts, timeout_s)
            if mount is None:
                print(
                    f"[sofle-flash] Error: Timed out waiting for {target.upper()} RPI-RP2 drive to mount.",
                    file=sys.stderr,
                )
                return False

        elif unresponsive > 0:
            unresponsive -= 1
            wait_manual_s = max(timeout_s, 45.0)
            if backend.prompt_manual_bootstrap is not None:
                backend.prompt_manual_bootstrap(target, wait_manual_s)
            else:
                print(
                    f"[sofle-flash] {target.upper()} half did not respond to remote identify (old firmware?).\n"
                    f"  Please hold BOOT while replugging {target.upper()} (within {int(wait_manual_s)}s)...",
                    flush=True,
                )
            mount = _wait_for_mount_appear(backend, existing_mounts, wait_manual_s)
            if mount is None:
                print(
                    f"[sofle-flash] Error: Timed out waiting for manual RPI-RP2 drive for {target.upper()}.",
                    file=sys.stderr,
                )
                return False

        elif trrs_detected and side == "both" and flashed_order:
            print(
                f"[sofle-flash] Note: {target.upper()} half is not attached via its own USB cable "
                "(TRRS split link active). Flash it by plugging USB directly into it and running '--side "
                f"{target}'.",
                flush=True,
            )
            break

        else:
            print(
                f"[sofle-flash] Error: Could not find {target.upper()} Sofle Pico half over USB Raw HID.",
                file=sys.stderr,
            )
            return False

        print(f"[sofle-flash] Writing {uf2_filename} to {mount} ({target.upper()})...", flush=True)
        backend.write_uf2(mount, uf2_bytes, uf2_filename)

        if not _wait_for_mount_disappear(backend, mount, timeout_s):
            print(f"[sofle-flash] Error: {mount} did not unmount after writing {target.upper()}.", file=sys.stderr)
            return False

        new_info = _wait_for_side_reenumerate(backend, target, prev_uptime, nonce_gen, timeout_s)
        if new_info is None:
            print(
                f"[sofle-flash] Error: {target.upper()} half did not come back on USB Raw HID after flashing.",
                file=sys.stderr,
            )
            return False

        if new_info.trrs_connected:
            trrs_detected = True
        flashed_order.append(target)
        print(
            f"[sofle-flash] [ok] {target.upper()} half flashed and verified (new uptime {new_info.uptime_ms} ms).",
            flush=True,
        )

    if not flashed_order:
        return False
    print(f"[sofle-flash] Done! Flashed: {', '.join(s.upper() for s in flashed_order)}.", flush=True)
    return True


def parse_args(argv: Optional[List[str]] = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Single-command sequential UF2 updater for Sofle Pico Left & Right halves."
    )
    parser.add_argument(
        "--build",
        action="store_true",
        default=False,
        help="Compile sofle_pico:default before flashing (default: flash existing .uf2 only)",
    )
    parser.add_argument(
        "--side",
        choices=("both", "left", "right"),
        default="both",
        help="Which half to flash (default: both, Left then Right)",
    )
    parser.add_argument(
        "--uf2",
        default=str(DEFAULT_UF2_PATH),
        help=f"Path to .uf2 firmware image (default: {DEFAULT_UF2_PATH})",
    )
    parser.add_argument(
        "--backend",
        choices=("auto", "hidraw", "win32", "hidapi"),
        default="auto",
        help="HID backend (default: hidraw on Linux, win32 on Windows, hidapi on macOS)",
    )
    parser.add_argument(
        "--timeout",
        type=float,
        default=20.0,
        help="Timeout in seconds for each bootloader/mount/reboot stage (default: 20.0)",
    )
    parser.add_argument(
        "--wsl-distro",
        default=None,
        help="Optional WSL distro name when invoking --build from Windows",
    )
    parser.add_argument(
        "--verbose",
        "-v",
        action="store_true",
        help="Print detailed diagnostic logs",
    )
    return parser.parse_args(argv)


def main(argv: Optional[List[str]] = None) -> int:
    args = parse_args(argv)
    uf2_path = Path(args.uf2)

    if args.build:
        try:
            run_build(REPO_ROOT, wsl_distro=args.wsl_distro)
        except Exception as exc:
            print(f"[sofle-flash] Error: {exc}", file=sys.stderr)
            return 1

    if not uf2_path.is_file():
        print(
            f"[sofle-flash] Error: Firmware file not found: {uf2_path} (run with --build to compile it).",
            file=sys.stderr,
        )
        return 1

    try:
        uf2_bytes = uf2_path.read_bytes()
        validate_uf2_bytes(uf2_bytes)
    except Exception as exc:
        print(f"[sofle-flash] Error: Invalid UF2 file '{uf2_path}': {exc}", file=sys.stderr)
        return 1

    delegated = maybe_delegate_wsl_to_windows(args, uf2_path)
    if delegated is not None:
        return delegated

    backend_name = sofle_sync.resolve_backend(args.backend)
    backend = create_flash_backend(backend_name)
    try:
        ok = flash_boards(
            backend=backend,
            uf2_bytes=uf2_bytes,
            uf2_filename=uf2_path.name,
            side=args.side,
            timeout_s=args.timeout,
            verbose=args.verbose,
        )
    except KeyboardInterrupt:
        print("\n[sofle-flash] Interrupted.", file=sys.stderr)
        return 130
    except Exception as exc:
        print(f"[sofle-flash] Error: {exc}", file=sys.stderr)
        return 1

    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
