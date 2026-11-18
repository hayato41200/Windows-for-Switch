#!/usr/bin/env bash
# R418H: the R411 UEFI (DSDT DISP + GPU0, Wi-Fi, audio, BT, USB hot-plug, R389 XHC0
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
set -euo pipefail

ROOT=/mnt/d/Codex/SwitchWOA-Bootstrap
REPO=/mnt/d/Codex/Windows-for-Switch-hekate
OUT="$ROOT/outputs/Switch-UEFI-R418H-HEKATE"
WORK="$ROOT/work/edk2"
PKG="$WORK/NintendoSwitchPkg"
R411="$ROOT/outputs/Switch-UEFI-R411-GPU"

# Files swapped into the work tree for this build: work-tree path -> source.
declare -A SWAP=(
  [Drivers/UsbPowerAuditDxe/UsbPowerAudit.c]="$REPO/Drivers/UsbPowerAuditDxe/UsbPowerAudit.c"
  [Drivers/UsbPowerAuditDxe/UsbPowerAudit.inf]="$REPO/Drivers/UsbPowerAuditDxe/UsbPowerAudit.inf"
  [Include/FwReleaseInfo.h]="$R411/FwReleaseInfo-R411.h"
  [AcpiTables/Dsdt/Dsdt.asl]="$REPO/AcpiTables/Dsdt/Dsdt.asl"
  [NintendoSwitch.dsc]="$REPO/NintendoSwitch.dsc"
  [NintendoSwitch.fdf]="$REPO/NintendoSwitch.fdf"
  [Include/Device/MemoryMap.h]="$REPO/Include/Device/MemoryMap.h"
  [Drivers/SdMmcDxe/SdMmc.c]="$REPO/Drivers/SdMmcDxe/SdMmc.c"
  [Drivers/SdMmcDxe/MmcHostOp.c]="$REPO/Drivers/SdMmcDxe/MmcHostOp.c"
  [Drivers/SdMmcDxe/EfiBlkDeviceOp.c]="$REPO/Drivers/SdMmcDxe/EfiBlkDeviceOp.c"
  [Drivers/SdMmcDxe/SdMmcDxe.inf]="$REPO/Drivers/SdMmcDxe/SdMmcDxe.inf"
  [Drivers/SdMmcDxe/Include/HostOp.h]="$REPO/Drivers/SdMmcDxe/Include/HostOp.h"
)
XHCI_SRC=("$R411/Xhci-R411.c" "$R411/XhciSched-R411.c" "$R411/UsbHcMem-R411.c")
XHCI_DST=("$WORK/MdeModulePkg/Bus/Pci/XhciDxe/Xhci.c" "$WORK/MdeModulePkg/Bus/Pci/XhciDxe/XhciSched.c" "$WORK/MdeModulePkg/Bus/Pci/XhciDxe/UsbHcMem.c")
FWDIR="$PKG/Firmware"
NEWLIB="$PKG/Library/SwitchPsciResetSystemLib"

test ! -e "$OUT"
test ! -e "$FWDIR"
test ! -e "$NEWLIB"
mkdir -p "$OUT/live-before-build"
for rel in "${!SWAP[@]}"; do test -f "$PKG/$rel" && test -f "${SWAP[$rel]}"; done
for i in 0 1 2; do test -f "${XHCI_SRC[$i]}" && test -f "${XHCI_DST[$i]}"; done

# Keep the live work-tree files and put them back afterwards, whatever happens.
for rel in "${!SWAP[@]}"; do mkdir -p "$OUT/live-before-build/$(dirname "$rel")"; cp "$PKG/$rel" "$OUT/live-before-build/$rel"; done
for i in 0 1 2; do cp "${XHCI_DST[$i]}" "$OUT/live-before-build/xhci-$i.c"; done
restore() {
  for rel in "${!SWAP[@]}"; do cp "$OUT/live-before-build/$rel" "$PKG/$rel"; done
  for i in 0 1 2; do cp "$OUT/live-before-build/xhci-$i.c" "${XHCI_DST[$i]}"; done
  rm -rf "$NEWLIB"
  test ! -e "$FWDIR"
}
trap restore EXIT HUP INT TERM

for rel in "${!SWAP[@]}"; do cp "${SWAP[$rel]}" "$PKG/$rel"; done
for i in 0 1 2; do cp "${XHCI_SRC[$i]}" "${XHCI_DST[$i]}"; done
cp -r "$REPO/Library/SwitchPsciResetSystemLib" "$NEWLIB"
mkdir -p "$OUT/inputs"
cp "$REPO/Drivers/UsbPowerAuditDxe/UsbPowerAudit.c" "$OUT/inputs/UsbPowerAudit-R418H.c"
cp "$REPO/AcpiTables/Dsdt/Dsdt.asl" "$OUT/inputs/Dsdt-R418H.asl"
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
test ! -e "$NEWLIB"
echo "EDK_SOURCE_RESTORE_MATCH=YES"
