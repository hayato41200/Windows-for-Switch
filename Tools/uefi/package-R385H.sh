#!/usr/bin/env bash
# R385H: the R379 UEFI built for hekate's L4T launcher (hekate -> BL31 -> bl33.bin),
# with the winx boot-chain changes (MemoryMap, FD base, hekate framebuffer, Null serial,
# winx SdMmcDxe). Output: bl33.bin (Tools/Bl33 stub + FD). No coreboot.
set -euo pipefail

ROOT=/mnt/d/Codex/SwitchWOA-Bootstrap
REPO=/mnt/d/Codex/Windows-for-Switch
OUT="$ROOT/outputs/Switch-UEFI-R385H-HEKATE"
WORK="$ROOT/work/edk2"
PKG="$WORK/NintendoSwitchPkg"
R379="$ROOT/outputs/Switch-UEFI-XUSB-R379-SENSORS"

# Files swapped into the work tree for this build: work-tree path -> source.
declare -A SWAP=(
  [Drivers/UsbPowerAuditDxe/UsbPowerAudit.c]="$R379/UsbPowerAudit-R379.c"
  [Drivers/UsbPowerAuditDxe/UsbPowerAudit.inf]="$R379/UsbPowerAudit-R379.inf"
  [Include/FwReleaseInfo.h]="$R379/FwReleaseInfo-R379.h"
  [AcpiTables/Dsdt/Dsdt.asl]="$R379/Dsdt-R379.asl"
  [NintendoSwitch.dsc]="$REPO/NintendoSwitch.dsc"
  [NintendoSwitch.fdf]="$REPO/NintendoSwitch.fdf"
  [Include/Device/MemoryMap.h]="$REPO/Include/Device/MemoryMap.h"
  [Drivers/SdMmcDxe/SdMmc.c]="$REPO/Drivers/SdMmcDxe/SdMmc.c"
  [Drivers/SdMmcDxe/MmcHostOp.c]="$REPO/Drivers/SdMmcDxe/MmcHostOp.c"
  [Drivers/SdMmcDxe/EfiBlkDeviceOp.c]="$REPO/Drivers/SdMmcDxe/EfiBlkDeviceOp.c"
  [Drivers/SdMmcDxe/SdMmcDxe.inf]="$REPO/Drivers/SdMmcDxe/SdMmcDxe.inf"
  [Drivers/SdMmcDxe/Include/HostOp.h]="$REPO/Drivers/SdMmcDxe/Include/HostOp.h"
)
XHCI_SRC=("$R379/Xhci-R379.c" "$R379/XhciSched-R379.c" "$R379/UsbHcMem-R379.c")
XHCI_DST=("$WORK/MdeModulePkg/Bus/Pci/XhciDxe/Xhci.c" "$WORK/MdeModulePkg/Bus/Pci/XhciDxe/XhciSched.c" "$WORK/MdeModulePkg/Bus/Pci/XhciDxe/UsbHcMem.c")
FWDIR="$PKG/Firmware"

test ! -e "$OUT"
test ! -e "$FWDIR"
mkdir -p "$OUT/live-before-build"
for rel in "${!SWAP[@]}"; do test -f "$PKG/$rel" && test -f "${SWAP[$rel]}"; done
for i in 0 1 2; do test -f "${XHCI_SRC[$i]}" && test -f "${XHCI_DST[$i]}"; done

# Keep the live work-tree files and put them back afterwards, whatever happens.
for rel in "${!SWAP[@]}"; do mkdir -p "$OUT/live-before-build/$(dirname "$rel")"; cp "$PKG/$rel" "$OUT/live-before-build/$rel"; done
for i in 0 1 2; do cp "${XHCI_DST[$i]}" "$OUT/live-before-build/xhci-$i.c"; done
restore() {
  for rel in "${!SWAP[@]}"; do cp "$OUT/live-before-build/$rel" "$PKG/$rel"; done
  for i in 0 1 2; do cp "$OUT/live-before-build/xhci-$i.c" "${XHCI_DST[$i]}"; done
  rm -rf "$FWDIR"
}
trap restore EXIT HUP INT TERM

for rel in "${!SWAP[@]}"; do cp "${SWAP[$rel]}" "$PKG/$rel"; done
for i in 0 1 2; do cp "${XHCI_SRC[$i]}" "${XHCI_DST[$i]}"; done
mkdir "$FWDIR"
cp "$R379/tegra21x_xusb_firmware" "$FWDIR/tegra21x_xusb_firmware"

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

python3 "$REPO/Tools/make-bl33.py" --fd "$OUT/TEGRA210_EFI.fd" --map "$OUT/FVMAIN_COMPACT.Fv.map" -o "$OUT/bl33.bin"
python3 "$REPO/Tools/make-bl33.py" --verify "$OUT/bl33.bin" --map "$OUT/FVMAIN_COMPACT.Fv.map"
sha256sum "$OUT/bl33.bin" | tee "$OUT/BL33-SHA256.txt"

# The work tree must be exactly as before.
for rel in "${!SWAP[@]}"; do cmp -s "$OUT/live-before-build/$rel" "$PKG/$rel"; done
for i in 0 1 2; do cmp -s "$OUT/live-before-build/xhci-$i.c" "${XHCI_DST[$i]}"; done
test ! -e "$FWDIR"
echo "EDK_SOURCE_RESTORE_MATCH=YES"
