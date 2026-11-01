#!/usr/bin/env bash
set -euo pipefail

ROOT=/mnt/d/Codex/SwitchWOA-Bootstrap
OUT="$ROOT/outputs/Switch-UEFI-R400-AUDIO"
WORK="$ROOT/work/edk2"
SRC="$WORK/NintendoSwitchPkg/Drivers/UsbPowerAuditDxe/UsbPowerAudit.c"
INF="$WORK/NintendoSwitchPkg/Drivers/UsbPowerAuditDxe/UsbPowerAudit.inf"
INFO="$WORK/NintendoSwitchPkg/Include/FwReleaseInfo.h"
XHCI="$WORK/MdeModulePkg/Bus/Pci/XhciDxe/Xhci.c"
SCHED="$WORK/MdeModulePkg/Bus/Pci/XhciDxe/XhciSched.c"
MEM="$WORK/MdeModulePkg/Bus/Pci/XhciDxe/UsbHcMem.c"
# R400: the published build. The DSC and FDF are swapped in like before, but
# EMBED_XUSB_FIRMWARE stays FALSE: no NVIDIA firmware goes into the image (the
# user puts tegra21x_xusb_firmware on the ESP). The build fails if it is there.
DSC="$WORK/NintendoSwitchPkg/NintendoSwitch.dsc"
FDF="$WORK/NintendoSwitchPkg/NintendoSwitch.fdf"
FWDIR="$WORK/NintendoSwitchPkg/Firmware"
FWBIN="$FWDIR/tegra21x_xusb_firmware"
# R400: the DSDT (adds the GPIO controller GPI0) is swapped in the same way.
DSDT="$WORK/NintendoSwitchPkg/AcpiTables/Dsdt/Dsdt.asl"
BASE="$OUT/coreboot-base-R343.rom"
CBF="$ROOT/work/coreboot-r234/build/cbfstool"
DEST=/mnt/d/Codex/coreboot-R400.rom

test -f "$SRC" && test -f "$INF" && test -f "$INFO" && test -f "$XHCI" && test -f "$SCHED" && test -f "$MEM" && test -f "$BASE"
test -f "$DSC" && test -f "$FDF" && test -f "$OUT/NintendoSwitch-R400.dsc" && test -f "$OUT/NintendoSwitch-R400.fdf"
test -f "$DSDT" && test -f "$OUT/Dsdt-R400.asl"
test ! -e "$FWDIR"
test -x "$CBF"
test ! -e "$OUT/coreboot-R400.rom"
test ! -e "$DEST"
test "$(stat -c %s "$BASE")" -eq 10485760

# Preserve the exact live inputs from this build, including any local edits.
cp "$SRC" "$OUT/UsbPowerAudit-live-before-build.c"
cp "$INF" "$OUT/UsbPowerAudit-live-before-build.inf"
cp "$INFO" "$OUT/FwReleaseInfo-live-before-build.h"
cp "$XHCI" "$OUT/Xhci-live-before-build.c"
cp "$SCHED" "$OUT/XhciSched-live-before-build.c"
cp "$MEM" "$OUT/UsbHcMem-live-before-build.c"
cp "$DSC" "$OUT/NintendoSwitch-live-before-build.dsc"
cp "$FDF" "$OUT/NintendoSwitch-live-before-build.fdf"
cp "$DSDT" "$OUT/Dsdt-live-before-build.asl"

restore_sources() {
  cp "$OUT/Dsdt-live-before-build.asl" "$DSDT"
  cmp -s "$OUT/Dsdt-live-before-build.asl" "$DSDT"
  cp "$OUT/NintendoSwitch-live-before-build.dsc" "$DSC"
  cp "$OUT/NintendoSwitch-live-before-build.fdf" "$FDF"
  rm -f "$FWBIN"
  if [ -d "$FWDIR" ]; then rmdir "$FWDIR"; fi
  cmp -s "$OUT/NintendoSwitch-live-before-build.dsc" "$DSC"
  cmp -s "$OUT/NintendoSwitch-live-before-build.fdf" "$FDF"
  test ! -e "$FWDIR"
  cp "$OUT/UsbPowerAudit-live-before-build.c" "$SRC"
  cp "$OUT/UsbPowerAudit-live-before-build.inf" "$INF"
  cp "$OUT/FwReleaseInfo-live-before-build.h" "$INFO"
  cp "$OUT/Xhci-live-before-build.c" "$XHCI"
  cp "$OUT/XhciSched-live-before-build.c" "$SCHED"
  cp "$OUT/UsbHcMem-live-before-build.c" "$MEM"
  cmp -s "$OUT/UsbPowerAudit-live-before-build.c" "$SRC"
  cmp -s "$OUT/UsbPowerAudit-live-before-build.inf" "$INF"
  cmp -s "$OUT/FwReleaseInfo-live-before-build.h" "$INFO"
  cmp -s "$OUT/Xhci-live-before-build.c" "$XHCI"
  cmp -s "$OUT/XhciSched-live-before-build.c" "$SCHED"
  cmp -s "$OUT/UsbHcMem-live-before-build.c" "$MEM"
}
trap restore_sources EXIT HUP INT TERM

sha256sum \
  "$WORK/MdeModulePkg/Bus/Pci/XhciDxe/XhciReg.c" \
  "$WORK/MdeModulePkg/Bus/Pci/XhciDxe/XhciSched.c" \
  "$WORK/MdeModulePkg/Bus/Pci/XhciDxe/UsbHcMem.c" \
  > "$OUT/EDK-XHCI-SOURCE-SHA256.txt"
sha256sum "$OUT/Xhci-R400.c" "$OUT/XhciSched-R400.c" "$OUT/UsbHcMem-R400.c" \
  > "$OUT/EDK-XHCI-R400-SHA256.txt"

cp "$OUT/UsbPowerAudit-R400.c" "$SRC"
cp "$OUT/UsbPowerAudit-R400.inf" "$INF"
cp "$OUT/FwReleaseInfo-R400.h" "$INFO"
cp "$OUT/Xhci-R400.c" "$XHCI"
cp "$OUT/XhciSched-R400.c" "$SCHED"
cp "$OUT/UsbHcMem-R400.c" "$MEM"
cp "$OUT/NintendoSwitch-R400.dsc" "$DSC"
cp "$OUT/NintendoSwitch-R400.fdf" "$FDF"
cp "$OUT/Dsdt-R400.asl" "$DSDT"

cd "$WORK"
set +u
source ./edksetup.sh
set -u
export GCC5_AARCH64_PREFIX=aarch64-linux-gnu-
build -a AARCH64 -p NintendoSwitchPkg/NintendoSwitch.dsc -t GCC5 2>&1 | tee "$OUT/BUILD-LOG.txt"
restore_sources

cp "$WORK/Build/NintendoSwitch-AARCH64/DEBUG_GCC5/FV/TEGRA210_EFI.fd" "$OUT/TEGRA210_EFI.fd"
cp "$WORK/Build/NintendoSwitch-AARCH64/DEBUG_GCC5/AARCH64/NintendoSwitchPkg/Drivers/UsbPowerAuditDxe/UsbPowerAudit/OUTPUT/UsbPowerAuditDxe.efi" "$OUT/UsbPowerAudit-R400.efi"
cp "$WORK/Build/NintendoSwitch-AARCH64/DEBUG_GCC5/AARCH64/MdeModulePkg/Bus/Pci/XhciDxe/XhciDxe/DEBUG/XhciDxe.debug" "$OUT/XhciDxe-R400.debug"
cp "$WORK/Build/NintendoSwitch-AARCH64/DEBUG_GCC5/AARCH64/MdeModulePkg/Bus/Pci/XhciDxe/XhciDxe/DEBUG/XhciDxe.map" "$OUT/XhciDxe-R400.map"
cp "$OUT/UsbPowerAudit-R400.c" "$OUT/UsbPowerAudit-R400-build-input.c"

aarch64-linux-gnu-objcopy -I binary -O elf64-littleaarch64 --binary-architecture aarch64 "$OUT/TEGRA210_EFI.fd" "$OUT/FD-R400.o"
aarch64-linux-gnu-ld -m aarch64elf "$OUT/FD-R400.o" -T NintendoSwitchPkg/Tools/FvWrapper.ld -o "$OUT/UEFI-R400.elf"
cp "$BASE" "$OUT/coreboot-R400.rom"
"$CBF" "$OUT/coreboot-R400.rom" remove -n fallback/payload
"$CBF" "$OUT/coreboot-R400.rom" add-payload -f "$OUT/UEFI-R400.elf" -n fallback/payload
"$CBF" "$OUT/coreboot-R400.rom" print
"$CBF" "$OUT/coreboot-R400.rom" extract -m arm64 -n fallback/payload -f "$OUT/payload-extracted.elf"

file "$OUT/UEFI-R400.elf" "$OUT/payload-extracted.elf"
readelf -h "$OUT/payload-extracted.elf" | grep -E 'Class:|Machine:|Entry point'
test "$(stat -c %s "$OUT/coreboot-R400.rom")" -eq 10485760
sha256sum "$OUT/coreboot-R400.rom" > "$OUT/ROM-SHA256.txt"
ROM_HASH=$(awk '{print $1}' "$OUT/ROM-SHA256.txt")
install -m 0644 "$OUT/coreboot-R400.rom" "$DEST"
test "$(sha256sum "$DEST" | awk '{print $1}')" = "$ROM_HASH"
printf 'ROM copied to %s\nSHA-256: %s\nROM size: %s bytes\n' "$DEST" "$ROM_HASH" "$(stat -c %s "$DEST")"

if cmp -s "$OUT/UsbPowerAudit-live-before-build.c" "$SRC" &&
   cmp -s "$OUT/UsbPowerAudit-live-before-build.inf" "$INF" &&
   cmp -s "$OUT/FwReleaseInfo-live-before-build.h" "$INFO" &&
   cmp -s "$OUT/Xhci-live-before-build.c" "$XHCI" &&
   cmp -s "$OUT/XhciSched-live-before-build.c" "$SCHED" &&
   cmp -s "$OUT/UsbHcMem-live-before-build.c" "$MEM" &&
   cmp -s "$OUT/NintendoSwitch-live-before-build.dsc" "$DSC" &&
   cmp -s "$OUT/NintendoSwitch-live-before-build.fdf" "$FDF" &&
   cmp -s "$OUT/Dsdt-live-before-build.asl" "$DSDT" &&
   test ! -e "$FWDIR"; then
  echo "EDK_SOURCE_RESTORE_MATCH=YES"
else
  echo "EDK_SOURCE_RESTORE_MATCH=NO" >&2
  exit 1
fi
