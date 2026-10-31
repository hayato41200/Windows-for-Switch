# XUSB firmware (not included)

The Tegra210 XUSB (USB host) firmware is NVIDIA's and is not stored in this repository.

1. Get `nvidia/tegra210/xusb.bin` from
   [linux-firmware](https://git.kernel.org/pub/scm/linux/kernel/git/firmware/linux-firmware.git)
   (126,464 bytes; accepted since R392, not yet tested on hardware),
   or `tegra21x_xusb_firmware` from an L4T / Switchroot image (124,416 bytes, tested,
   SHA-256 `81aa8787…5d40`).
2. Since R392 (default): copy it to the root of the SD card's ESP (the FAT partition)
   as `tegra21x_xusb_firmware`. The UEFI reads it at every boot. Windows protects the
   ESP, so copy it from an elevated prompt after giving the partition a drive letter.
3. Private builds only: save it here as `Firmware/tegra21x_xusb_firmware` and build
   with `-D EMBED_XUSB_FIRMWARE=TRUE` to embed it in the image.

The file name is listed in `.gitignore`. Follow the license that ships with the
firmware if you redistribute a built image.
