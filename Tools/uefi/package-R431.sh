#!/usr/bin/env bash
# R431 (RCM / coreboot chain): the same sources as R431H, built WITHOUT -D SWITCH_HEKATE=1
# and put into the R343 coreboot base ROM as fallback/payload (as package-R411.sh).
# A check ROM so main can take the hekate work: the RCM chain has only been compile-checked
# (R421). Not for distribution (the coreboot part carries the console's memory-training data).
# No NVIDIA firmware in the image (EMBED_XUSB_FIRMWARE stays off).
# R431: + USB-C DisplayPort Alt Mode stage 1 (see package-R431H.sh). Local check ROM only.
set -euo pipefail

ROOT=/mnt/d/Codex/SwitchWOA-Bootstrap
REPO="${REPO:-/mnt/d/Codex/Windows-for-Switch}"
OUT="$ROOT/outputs/Switch-UEFI-R431-RCM"
BASE="$ROOT/outputs/Switch-UEFI-R408-DISPLAY/coreboot-base-R343.rom"
CBF="$ROOT/work/coreboot-r234/build/cbfstool"
DEST=/mnt/d/Codex/coreboot-R431.rom
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
test ! -e "$DEST"
test -f "$BASE" && test -x "$CBF"
test ! -e "$FWDIR"
for d in "${NEWDIRS[@]}"; do test ! -e "$PKG/$d"; done
test ! -e "$PKG/Application"
mkdir -p "$OUT/live-before-build"
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
cp "$REPO/Drivers/UsbPowerAuditDxe/UsbPowerAudit.c" "$OUT/inputs/UsbPowerAudit-R431.c"
cp "$REPO/AcpiTables/Dsdt/Dsdt.asl" "$OUT/inputs/Dsdt-R431.asl"
sha256sum "$OUT/inputs/"* > "$OUT/inputs/SHA256.txt"

cd "$WORK"
set +u
source ./edksetup.sh
set -u
export GCC5_AARCH64_PREFIX=aarch64-linux-gnu-
build -a AARCH64 -p NintendoSwitchPkg/NintendoSwitch.dsc -t GCC5 2>&1 | tee "$OUT/BUILD-LOG.txt"
test "${PIPESTATUS[0]}" -eq 0

B="$WORK/Build/NintendoSwitch-AARCH64/DEBUG_GCC5"
cp "$B/FV/TEGRA210_EFI.fd" "$OUT/TEGRA210_EFI.fd"
cp "$B/FV/FVMAIN_COMPACT.Fv.map" "$OUT/FVMAIN_COMPACT.Fv.map" 2>/dev/null || true
restore
trap - EXIT HUP INT TERM

# The firmware must not be in the image.
if grep -c "5A0F6A3E-2D4C-4C1B-9E51-7B3F168A0C42" "$OUT/BUILD-LOG.txt" >/dev/null; then echo "XUSB firmware FFS in the build" >&2; exit 1; fi

cd "$WORK"
aarch64-linux-gnu-objcopy -I binary -O elf64-littleaarch64 --binary-architecture aarch64 "$OUT/TEGRA210_EFI.fd" "$OUT/FD-R431.o"
aarch64-linux-gnu-ld -m aarch64elf "$OUT/FD-R431.o" -T NintendoSwitchPkg/Tools/FvWrapper.ld -o "$OUT/UEFI-R431.elf"
cp "$BASE" "$OUT/coreboot-R431.rom"
"$CBF" "$OUT/coreboot-R431.rom" remove -n fallback/payload
"$CBF" "$OUT/coreboot-R431.rom" add-payload -f "$OUT/UEFI-R431.elf" -n fallback/payload
"$CBF" "$OUT/coreboot-R431.rom" print
test "$(stat -c %s "$OUT/coreboot-R431.rom")" -eq 10485760
sha256sum "$OUT/coreboot-R431.rom" | tee "$OUT/ROM-SHA256.txt"
install -m 0644 "$OUT/coreboot-R431.rom" "$DEST"
cmp "$OUT/coreboot-R431.rom" "$DEST"

# The work tree must be exactly as before.
for rel in "${!SWAP[@]}"; do cmp -s "$OUT/live-before-build/$rel" "$PKG/$rel"; done
for i in 0 1 2; do cmp -s "$OUT/live-before-build/xhci-$i.c" "${XHCI_DST[$i]}"; done
test ! -e "$FWDIR"
for d in "${NEWDIRS[@]}"; do test ! -e "$PKG/$d"; done
test ! -e "$PKG/Application"
echo "EDK_SOURCE_RESTORE_MATCH=YES"
