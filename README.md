# Windows for Switch

![Windows for Switch](docs/images/social-preview.png)

Windows 10 on ARM (Windows 10 IoT Enterprise LTSC 2021, build 19044, ARM64) on the
original Nintendo Switch (Tegra210 "Erista", unpatched units that can enter RCM),
booted from microSD with coreboot + EDK2 UEFI.

This repository is a continuation of
[imbushuo/NintendoSwitchPkg](https://github.com/imbushuo/NintendoSwitchPkg) (the
EDK2 platform package, whose history is kept here). It adds:

- the UEFI changes needed to hand USB, power and the panel over to Windows;
- a set of Windows kernel drivers for the Switch hardware;
- notes from bring-up.

日本語版: [README-ja.md](README-ja.md)

> **Warning:** Experimental. It runs untested power, clock and display code on real
> hardware. Nothing here writes to the eMMC, but use it at your own risk. CPU
> profiles above the default raise VDD_CPU, so use them at your own risk too.

## What works

| Feature | How |
|---|---|
| Boot to the desktop from microSD | RCM → coreboot → UEFI (this package) → Windows Boot Manager on the SD |
| USB host: HS and FS, hubs, keyboard, mouse, storage, RTL8153 Ethernet | UEFI brings XUSB up and leaves it running; Windows inbox `USBXHCI` |
| USB-C hot-plug under Windows (device: VBUS on/off; charger hub: charge + host) | UEFI R378 starts XUSB even with nothing attached; `switchbattery.sys` v4 watches the BM92T |
| Charging through a USB-C PD hub | UEFI: BM92T sink + BQ24193 charge mode + DR_SWAP to host |
| CPU at 1020 MHz at boot; selectable 614–1766 MHz in Windows | UEFI (PLLX + MAX77621) + `switchcpu.sys` + tray app |
| Fan (temperature controlled) | UEFI + `switchfan.sys` |
| Backlight brightness; off when Windows turns the display off | UEFI R376 (PWM0) + `switchfan.sys` v2 + tray app |
| Volume buttons | DSDT `ACPI0011` + GPIO driver + inbox `hidinterrupt` |
| Power button (display off / lock) | `switchpwrbtn.sys` (MAX77620 EN0) |
| Battery level, AC, charging | `switchbattery.sys` (MAX17050 + BQ24193) |
| Touch screen: multi-touch, press-and-hold | UEFI powers the FTM4 + `switchtouch.sys` |
| Clock kept across reboots | `switchrtc.sys` reads the MAX77620 RTC (never writes it, Horizon keeps using it) and applies an offset learned at the last time sync |

Not done: GPU acceleration (basic display only), audio, Wi-Fi/BT, Joy-Con rail, sleep.

## Repository layout

| Path | Contents |
|---|---|
| `AcpiTables/`, `Drivers/`, `Library/`, `NintendoSwitch.dsc/.fdf` | The EDK2 package. `Drivers/UsbPowerAuditDxe` does the board bring-up (USB/XUSB, PD and charging, CPU clock, fan, backlight, touch power). |
| `Firmware/` | Put the XUSB firmware here (not included; see its README). |
| `patches/edk2/` | One fix for edk2 `XhciDxe`: allocate non-coherent DMA buffers write-combined. |
| `patches/coreboot/` | Build fixes for [imbushuo/Coreboot](https://github.com/imbushuo/Coreboot) with current toolchains. |
| `Tools/uefi/package-R378.sh` | The script the tested image was built with (paths are those of the original setup). |
| `Tools/rcm-mac/` | Sending the payload from macOS with shofel2. |
| `windows/` | Windows drivers: sources (`Tegra210Gpio`, `Tegra210I2c`, `Switch*`), installable kits (`Switch-*`, test-signed), the build script, the tray app and the login fix. |
| `docs/ja/uefi-revisions/` | Per-revision notes and hardware results for the UEFI test images (Japanese). |

## Building the UEFI payload

1. Check out edk2 `edk2-stable202102`. Apply `patches/edk2/*.patch` (`patch -p1`).
2. Put this repository at `edk2/NintendoSwitchPkg`. Add the XUSB firmware as described
   in `Firmware/README.md`.
3. Build `NintendoSwitchPkg/NintendoSwitch.dsc` for AARCH64 with GCC5.
4. Wrap `TEGRA210_EFI.fd` as an ELF payload and add it to a coreboot ROM built from
   imbushuo/Coreboot (with `patches/coreboot`), as `fallback/payload`.
   - `Tools/uefi/package-R378.sh` shows the exact commands: `objcopy` + `FvWrapper.ld` + `cbfstool`.
5. Send the ROM in RCM with shofel2. From Linux:
   `shofel2.py cbfs.bin coreboot.rom`. For macOS, see `Tools/rcm-mac/`.

## Installing the Windows drivers

The drivers are KMDF 1.15, built with LLVM (clang-cl, lld-link) against the WDK
10.0.19041 headers; see `windows/tools/Build-Driver.ps1`.

1. Enable test signing from a PC with `windows/Switch-Stage0/Set-SwitchTestSigning.ps1`.
   It edits the BCD on the SD with `bcdedit /store`.
2. Import `windows/tools/cert/SwitchWOA-Test.cer` on the Switch.
   - It goes into Trusted Root and Trusted Publishers.
   - Or rebuild the drivers and sign them with your own certificate.
3. Install the drivers in this order. Each `Switch-*` folder has a script and a README.
   1. GPIO (`Switch-Stage1`)
   2. I2C (`Switch-Stage3`)
   3. Battery
   4. Fan / backlight
   5. Touch
   6. Power button
   7. CPU clock
   8. RTC (then run `Rtc.ps1 -Mode Sync` once with the network up)
4. Optional: run `windows/Switch-ClockTray/SwitchClock.exe` from the notification area.
   It selects the CPU profile and the screen brightness.

On Windows 10 ARM64, Windows PowerShell 5.1 runs as x86. The scripts therefore reach
`pnputil`/`bcdedit` through `%windir%\Sysnative`.

## Credits

- [imbushuo](https://github.com/imbushuo): NintendoSwitchPkg and the Switch coreboot.
- [CTCaer / Hekate](https://github.com/CTCaer/hekate) and Switchroot (L4T): register
  sequences for the fan, touch panel, backlight, PMIC and charger.
- The Linux kernel Tegra drivers and device trees, e.g. the CPU CVB tables.
- fail0verflow: shofel2.

## License

GPL-2.0, the same as NintendoSwitchPkg (see `LICENSE`). Parts of the bring-up code
follow Hekate and Linux, which are also GPL-2.0.
