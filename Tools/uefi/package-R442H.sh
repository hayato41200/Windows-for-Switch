#!/usr/bin/env bash
# R441H: the R411 UEFI (DSDT DISP + GPU0, Wi-Fi, audio, BT, USB hot-plug, R389 XHC0
# gate / PWM clock / button debounce) built for hekate's L4T launcher
# (hekate -> BL31 -> bl33.bin) with -D SWITCH_HEKATE=1, plus R412: start PLLU
# and set the UTMIP PLL as coreboot does when hekate left them off, and name
# the real USB stop point in the summary. Output: bl33.bin (Tools/Bl33 stub + FD).
# No coreboot. No NVIDIA firmware in the image (EMBED_XUSB_FIRMWARE stays off;
# UsbPowerAuditDxe reads \tegra21x_xusb_firmware from the ESP as with R403+).
# R413: release the XUSB_PADCTL reset hekate leaves set (the first PADCTL read
# raised an SError in R412H).
# R414: gate the XUSB partitions hekate leaves on (R243 baseline), and keep
# hekate's UTMIP PLL when it already set USB up.
# R415: SD BlockIo read-only again, as in the coreboot chain (BCD 0xc000000f on hekate).
# R416: ResetSystem = PSCI + backlight-off marker (Library/SwitchPsciResetSystemLib), to see
# whether Windows reaches the EFI ResetSystem on restart (reboot-to-payload).
# R417: show at boot whether that marker got its runtime MMIO mapping.
# R418: the bl33 stub puts the backlight PWM clock back on CLK_M (hekate -> BL31 leaves
# CLK_SOURCE_PWM = 0x80000004, CLK_32K: ~43 Hz flicker until R389), and records what it found.
# R419: the real cause: ClockManagementDxe (u-boot periph_clk_init_table) muxed the running
# backlight PWM to the 32.768 kHz clock. The hekate build leaves the PWM clock alone.
# R420: the side buttons are a UEFI keyboard (SidebandButtonDxe: VOL+ Up, VOL- Down,
# power Enter / held Esc) on ConIn, and VOL+ (Up) is a boot-menu hot key next to ESC/F2.
# R421: SwitchTouchDxe (touch screen as EFI_ABSOLUTE_POINTER, framebuffer coordinates) and
# SwitchMenuApp (GOP touch menu: Boot Windows / Return to hekate / UEFI settings / Power off),
# opened by VOL+ instead of UiApp and when nothing boots.
# R422: the menu redrawn in a Windows 11 style (Draw.c: glass cards, glow, light ribbons,
# Noto Sans JP glyphs and icons from Tools/menu-assets/make_assets.py).
# R423: release build: no touch marker, the R416/R417 reset marker removed (plain PSCI
# ResetSystem again: switchpwrbtn does restart/power-off from Windows). REPO can be set
# from outside (default: the main checkout).
# R424: USB-C DisplayPort Alt Mode stage 1 in UsbPowerAuditDxe (BM92T: Nintendo dock
# alt mode, DP configure + HPD, dock LED and USB hub). Only on the sink path (a power
# source attached, Switch DFP); nothing on the display side yet. Build from the dpalt
# worktree: REPO=/mnt/d/Codex/Windows-for-Switch-dpalt.
# R425: DP Alt Mode also for non-Nintendo DP adapters/docks (generic DP enter as L4T,
# plug/receptacle pin field, no dock VDMs).
# R426: DP Alt Mode stage 2a: DPAUX1 clock/reset/pad, HPD from AUXSTAT, DPCD and EDID over AUX.
# R427: DP Alt Mode stage 2b: PLLDP, SOR1 clock/pads/lanes, DP link training (no video).
# R428: PLLDP reprogrammed to 270 MHz when an earlier loader left another setting.
# R429: DP Alt stage 3 - DC-B (PLLP, 1280x720@60 background colour) attached to SOR1.
# R430: disp2 clock source register fixed (0x13C), DC-B frame-end and DPCD sink-status checks.
# R431: converter identity (DPCD 0x500) and a 20 s white hold with per-second status.
# R432: a hub answering as the Nintendo dock enters standard DP Alt Mode (FF01) when offered.
# R433: DC-B clock from PLLD2 74.25 MHz, shift divider 1, measured frame rate.
# R434: the UEFI screen mirrored on the TV via DC-B window A (same framebuffer, scaled).
# R435: no colour sequence and no 20 s hold; DC-B keeps scanning for Windows (switchdisplay 1.3.0).
# R436: 15 s hold with the reason when a partner is attached but the TV was not started.
# R437: when DP Alt was tried, print the disp2 clock and DC-B state at the end of UEFI, hold 15 s.
# R438: one line per DPCD-setup step after the SOR1 pads come up (R437H stopped there).
# R439: SOR1 pad power - MAX77620 LDO8 vdd-hdmi-dp 1.05 V on and HDMI pad DPD off before R427.
# R440: LDO8 on at every boot (Windows hot-plug), stop before the SOR1 pad clock when TERMADJ is 15.
# R441: release build of the DP Alt work: no 15 s holds at the end of UEFI.
# R442: the UEFI version in SMBIOS (BIOS version "1.09 R442H (commit)"), for SwitchCenter.
set -euo pipefail

ROOT=/mnt/d/Codex/SwitchWOA-Bootstrap
REPO="${REPO:-/mnt/d/Codex/Windows-for-Switch}"
OUT="$ROOT/outputs/Switch-UEFI-R442H-HEKATE"
WORK="$ROOT/work/edk2"
PKG="$WORK/NintendoSwitchPkg"
R411="$ROOT/outputs/Switch-UEFI-R411-GPU"

# Files swapped into the work tree for this build: work-tree path -> source.
declare -A SWAP=(
  [Drivers/UsbPowerAuditDxe/UsbPowerAudit.c]="$REPO/Drivers/UsbPowerAuditDxe/UsbPowerAudit.c"
  [Drivers/UsbPowerAuditDxe/UsbPowerAudit.inf]="$REPO/Drivers/UsbPowerAuditDxe/UsbPowerAudit.inf"
  [Include/FwReleaseInfo.h]="$OUT/FwReleaseInfo.h"
  [Drivers/SmBiosTableDxe/SmBiosTableDxe.c]="$REPO/Drivers/SmBiosTableDxe/SmBiosTableDxe.c"
  [AcpiTables/Dsdt/Dsdt.asl]="$REPO/AcpiTables/Dsdt/Dsdt.asl"
  [NintendoSwitch.dsc]="$REPO/NintendoSwitch.dsc"
  [NintendoSwitch.fdf]="$REPO/NintendoSwitch.fdf"
  [Include/Device/MemoryMap.h]="$REPO/Include/Device/MemoryMap.h"
  [Drivers/SdMmcDxe/SdMmc.c]="$REPO/Drivers/SdMmcDxe/SdMmc.c"
  [Drivers/SdMmcDxe/MmcHostOp.c]="$REPO/Drivers/SdMmcDxe/MmcHostOp.c"
  [Drivers/SdMmcDxe/EfiBlkDeviceOp.c]="$REPO/Drivers/SdMmcDxe/EfiBlkDeviceOp.c"
  [Drivers/SdMmcDxe/SdMmcDxe.inf]="$REPO/Drivers/SdMmcDxe/SdMmcDxe.inf"
  [Drivers/SdMmcDxe/Include/HostOp.h]="$REPO/Drivers/SdMmcDxe/Include/HostOp.h"
  [Drivers/ClockManagementDxe/Tegra210/clock.c]="$REPO/Drivers/ClockManagementDxe/Tegra210/clock.c"
  [Drivers/SidebandButtonDxe/SidebandButtons.c]="$REPO/Drivers/SidebandButtonDxe/SidebandButtons.c"
  [Drivers/SidebandButtonDxe/SidebandButtonDxe.inf]="$REPO/Drivers/SidebandButtonDxe/SidebandButtonDxe.inf"
  [Library/PlatformBootManagerLib/PlatformBm.c]="$REPO/Library/PlatformBootManagerLib/PlatformBm.c"
)
XHCI_SRC=("$R411/Xhci-R411.c" "$R411/XhciSched-R411.c" "$R411/UsbHcMem-R411.c")
XHCI_DST=("$WORK/MdeModulePkg/Bus/Pci/XhciDxe/Xhci.c" "$WORK/MdeModulePkg/Bus/Pci/XhciDxe/XhciSched.c" "$WORK/MdeModulePkg/Bus/Pci/XhciDxe/UsbHcMem.c")
FWDIR="$PKG/Firmware"
NEWDIRS=(Drivers/SwitchTouchDxe Application/SwitchMenuApp)

test ! -e "$OUT"
test ! -e "$FWDIR"
for d in "${NEWDIRS[@]}"; do test ! -e "$PKG/$d"; done
test ! -e "$PKG/Application"
mkdir -p "$OUT/live-before-build"
{ sed '/^#endif/d' "$R411/FwReleaseInfo-R411.h"; echo '#define __SWOA_UEFI_VERSION__ "R442H"'; echo '#endif'; } > "$OUT/FwReleaseInfo.h"
for rel in "${!SWAP[@]}"; do test -f "$PKG/$rel" && test -f "${SWAP[$rel]}"; done
for i in 0 1 2; do test -f "${XHCI_SRC[$i]}" && test -f "${XHCI_DST[$i]}"; done

# Keep the live work-tree files and put them back afterwards, whatever happens.
for rel in "${!SWAP[@]}"; do mkdir -p "$OUT/live-before-build/$(dirname "$rel")"; cp "$PKG/$rel" "$OUT/live-before-build/$rel"; done
for i in 0 1 2; do cp "${XHCI_DST[$i]}" "$OUT/live-before-build/xhci-$i.c"; done
restore() {
  for rel in "${!SWAP[@]}"; do cp "$OUT/live-before-build/$rel" "$PKG/$rel"; done
  for i in 0 1 2; do cp "$OUT/live-before-build/xhci-$i.c" "${XHCI_DST[$i]}"; done
  for d in "${NEWDIRS[@]}"; do rm -rf "$PKG/$d"; done
  rmdir "$PKG/Application" 2>/dev/null || true
  test ! -e "$FWDIR"
}
trap restore EXIT HUP INT TERM

for rel in "${!SWAP[@]}"; do cp "${SWAP[$rel]}" "$PKG/$rel"; done
for i in 0 1 2; do cp "${XHCI_SRC[$i]}" "${XHCI_DST[$i]}"; done
for d in "${NEWDIRS[@]}"; do mkdir -p "$PKG/$(dirname "$d")"; cp -r "$REPO/$d" "$PKG/$d"; done
mkdir -p "$OUT/inputs"
cp "$REPO/Drivers/UsbPowerAuditDxe/UsbPowerAudit.c" "$OUT/inputs/UsbPowerAudit-R442H.c"
cp "$REPO/AcpiTables/Dsdt/Dsdt.asl" "$OUT/inputs/Dsdt-R442H.asl"
sha256sum "$OUT/inputs/"* > "$OUT/inputs/SHA256.txt"

cd "$WORK"
set +u
source ./edksetup.sh
set -u
export GCC5_AARCH64_PREFIX=aarch64-linux-gnu-
build -a AARCH64 -p NintendoSwitchPkg/NintendoSwitch.dsc -t GCC5 -D SWITCH_HEKATE=1 2>&1 | tee "$OUT/BUILD-LOG.txt"
test "${PIPESTATUS[0]}" -eq 0

B="$WORK/Build/NintendoSwitch-AARCH64/DEBUG_GCC5"
cp "$B/FV/TEGRA210_EFI.fd" "$OUT/TEGRA210_EFI.fd"
cp "$B/FV/FVMAIN_COMPACT.Fv.map" "$OUT/FVMAIN_COMPACT.Fv.map" 2>/dev/null || true
restore
trap - EXIT HUP INT TERM

# The firmware must not be in the image.
if grep -c "5A0F6A3E-2D4C-4C1B-9E51-7B3F168A0C42" "$OUT/BUILD-LOG.txt" >/dev/null; then echo "XUSB firmware FFS in the build" >&2; exit 1; fi

python3 "$REPO/Tools/make-bl33.py" --fd "$OUT/TEGRA210_EFI.fd" --map "$OUT/FVMAIN_COMPACT.Fv.map" -o "$OUT/bl33.bin"
python3 "$REPO/Tools/make-bl33.py" --verify "$OUT/bl33.bin" --map "$OUT/FVMAIN_COMPACT.Fv.map"
sha256sum "$OUT/bl33.bin" | tee "$OUT/BL33-SHA256.txt"

# The work tree must be exactly as before.
for rel in "${!SWAP[@]}"; do cmp -s "$OUT/live-before-build/$rel" "$PKG/$rel"; done
for i in 0 1 2; do cmp -s "$OUT/live-before-build/xhci-$i.c" "${XHCI_DST[$i]}"; done
test ! -e "$FWDIR"
for d in "${NEWDIRS[@]}"; do test ! -e "$PKG/$d"; done
test ! -e "$PKG/Application"
echo "EDK_SOURCE_RESTORE_MATCH=YES"
