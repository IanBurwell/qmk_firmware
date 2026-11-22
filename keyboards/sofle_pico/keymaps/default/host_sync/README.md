# Sofle Pico Dual-USB Sync Service (Linux & Windows)

Lightweight host relay that synchronizes **`_LOWER` / `_RAISE` / `_ADJUST` layers (~1–2 ms latency)**, **modifier OLED badges (`S C A G`)**, **GP24 OLED/RGB lights toggle**, and **RGB Matrix profile/HSV/speed** between both halves of your Sofle Pico when plugged in via two USB cables (no TRRS cable).

- **Works automatically**: When TRRS is plugged in (`0:TRRS`), QMK's native split link handles everything and the service stays idle. When TRRS is unplugged and both USB cables are plugged in, the OLED header switches from `0:N/C` → `0:WAIT` → `0:SYNC` once both halves are paired.
- **No VIA conflict**: Uses custom Raw HID command `0xF0`; normal VIA configuration continues to work untouched.
- **Standalone fallback**: If plugged into a machine without the service (`0:N/C` / `0:WAIT`), both halves still work as independent USB keyboards with fixed Left/Right RGB initialization and native OS cross-half typing modifiers (`Shift`/`Ctrl`/`Alt`/`GUI`).

---

## Single-Command Dual-Board Flashing (`sofle_flash.py`)

When both halves are plugged in via USB, you can update both boards sequentially with a single command (zero external dependencies; works cleanly even while `sofle_sync.py` is running in the background):

- **Flash existing `.uf2` build** (Windows PowerShell, WSL bash, or native Linux):
  ```powershell
  python.exe keyboards\sofle_pico\keymaps\default\host_sync\sofle_flash.py
  ```
  ```bash
  python3 keyboards/sofle_pico/keymaps/default/host_sync/sofle_flash.py
  ```

- **Compile + flash both boards in one command** (`--build`):
  ```powershell
  python.exe keyboards\sofle_pico\keymaps\default\host_sync\sofle_flash.py --build
  ```
  ```bash
  python3 keyboards/sofle_pico/keymaps/default/host_sync/sofle_flash.py --build
  ```

- **Useful options**:
  - `--side {both,left,right}`: flash only one half or both (default: `both`, Left then Right).
  - `--uf2 PATH`: flash a custom `.uf2` file instead of `sofle_pico_default.uf2`.
  - `--verbose` / `-v`: print probe, ACK, and reboot progress details.

> **One-time bootstrap:** If a half is still running older firmware from before remote bootloader support (`0x03`/`0x04`) was flashed, `sofle_flash.py` will prompt you once to hold `BOOT` while plugging that half in and will then flash it automatically. Every update after that is completely hands-free.

---

## Linux Setup (Pure Python Stdlib, No `pip` Required)

1. Install the udev rule so your user can access `/dev/hidraw*` without root:
   ```bash
   sudo cp keyboards/sofle_pico/keymaps/default/host_sync/99-sofle-pico-hidraw.rules /etc/udev/rules.d/
   sudo udevadm control --reload-rules && sudo udevadm trigger
   ```
   *(Unplug and replug the USB cables if the keyboards were already attached.)*

2. Test interactively:
   ```bash
   python3 keyboards/sofle_pico/keymaps/default/host_sync/sofle_sync.py --verbose
   ```

3. Enable auto-start at login via `systemd --user`:
   ```bash
   mkdir -p ~/.config/systemd/user
   cp keyboards/sofle_pico/keymaps/default/host_sync/sofle-pico-sync.service ~/.config/systemd/user/
   systemctl --user daemon-reload
   systemctl --user enable --now sofle-pico-sync.service
   ```

---

## Windows Setup (Pure Python Stdlib, No Packages Required)

1. Ensure Python 3.9+ (`python.exe`) is installed (uses built-in Windows HID APIs via `ctypes` — no `pip install` packages required).

2. Test in PowerShell:
   ```powershell
   python.exe keyboards\sofle_pico\keymaps\default\host_sync\sofle_sync.py --verbose
   ```
   *(Optional fallback: `pip install hidapi` and run with `--backend hidapi` if needed.)*

3. Run silently in the background on login:
   - Press `Win + R`, run `shell:startup`, and create a shortcut pointing to `sofle_sync_windows.vbs` (or configure a Task Scheduler task running `pythonw.exe <path>\sofle_sync.py` at logon).
