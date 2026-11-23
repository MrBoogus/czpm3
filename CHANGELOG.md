# Changelog

## v1.4.2 (2026-11-24)
- **BT & Fork Hardening**: Hardened `start_bt_pair()` trust ordering, PIN prompt matching, cleanup (`untrust`/`remove`), and `pairable off`; resolved executables and built `argv`/`envp` pre-`fork()`/`forkpty()`, added `5000 ms` `poll()` timeouts with `SIGTERM`/`SIGKILL` cleanup, offloaded UI-thread blocking calls, and enforced `O_CLOEXEC`/`FD_CLOEXEC` and `master_fd_` lock safety.
- **Wi-Fi Recovery & Write Guard**: Persisted `~/.config/czpm3/wifi_paused` with automatic startup/exit recovery and a `Restore Wi-Fi` menu row; removed non-Proxmark tty fallback in `scan_ports()`; added case-insensitive CLI confirmation (`Confirm write? Y/N`) before write/clone/sim/flash commands.
- **Packaging & BLE Patch**: Updated `packaging/build-deb.sh` to set `cap_net_raw,cap_net_admin+ep`, `chgrp bluetooth` + `chmod 0750 /usr/bin/proxmark3`, require `libcap2-bin`, and install DEP-5 `/usr/share/doc/czpm3/copyright` (tested upstream `da929f9`) plus `pm3-ble-cpz.patch`; guarded `OCF 0x0032` (`LE Set PHY`) in `patches/pm3-ble-cpz.patch` behind `OCF_LE_READ_LOCAL_SUPPORTED_FEATURES` LE 2M support check.

## v1.4.1 (2026-11-24)
- Renamed package and application branding to **CZPM3** (`czpm3`).
- Automatic Wi-Fi coexistence management (`nmcli radio wifi`) on CardputerZero's shared 2.4 GHz `CYW4343A2` (`btc_mode=0`): pauses Wi-Fi automatically during `bt:` pairing and RDV4 BlueShark Classic BT sessions, then restores Wi-Fi when switching to BLE/USB/Offline or exiting.
- Kept PM5 BLE (`ble:`) running concurrently with active Wi-Fi.

## v1.4.0 (2026-11-24)
- Enlarged framebuffer font sizes across Home (`14 px` bold), Submenus (`13 px` bold + `11 px` wrapped descriptions), Output/CLI (`11 px` default), and `14x14` monochrome pixel icons (with `W` toggle to `9 px` compact mode).
- Added RDV4 BlueShark Classic Bluetooth (`bt:`) discovery, non-root PIN `1234` pairing (`bluetoothctl` PTY agent), `uart_posix.c` RFCOMM retry/`SO_RCVTIMEO`, and `comms.c` initial ping timeout handling.
- Fixed BlueZ 5.82 `scan bredr` / `scan le` scanning, deterministic device listing, and startup BT scan race.
- Added manual `Wi-Fi Radio (Off=BT)` toggle in `Device / BT`.

## v1.3.0 (2026-11-23)
- Implemented 4-mode non-blocking BlueZ BLE GATT RX timeout handling (`ble_posix.c` / `ble_posix.h`) fixing `hf mf autopwn` and multi-frame LF/T55xx dumps over Proxmark5 BWM BLE (`37-45 KB/s`).
- Improved framebuffer dirty-rect rendering, flat ASCII glyph cache, fast RGB565 blending (`0.000%` idle CPU), `Pm3Session` auto-reconnect, and graceful `Enter`/`Esc` stop for tuning commands.

## v1.2.0 - v1.2.1 (2026-11-23)
- Switched scroll/navigation bindings to CardputerZero APPLaunch keys (`F`/`X`/`Z`/`C` without holding `Fn`) and fixed command-line quote/prompt matching.

## v1.0.0 - v1.1.1 (2026-11-23)
- Initial native `320x170` framebuffer GUI, persistent `forkpty()` Proxmark3 session, Saved Dumps browser, CLI Console, and `.deb` packaging with `cap_net_raw,cap_net_admin+eip`.
