# R408: 画面のデバイス DISP(フレームバッファー無し、試験)

R405 に、DSDT のデバイス `DISP`(`ACPI\SWOA0E01`)を 1 つ足しただけの版。ほかの動きは R405 と同じ。

- 使うドライバー: `switchdisplay.sys` 1.0.2 以降(WDDM の表示専用ドライバー。試験キット `SwitchDisplay-1.0.2.0-testkit`)。
- `_CRS`:
  - DC-A `0x54200000`、`0x40000`
  - DSI-A `0x54300000`、`0x40000`
  - フレームバッファー(0xDFB80000)は**入れない**。R407 は入れたが、ドライバーが無くても Windows が起動しなくなった(画面がノイズになる)。
  - 割り込みはまだ無い(vblank は段階 3 で足す)。
- 確かめること:
  1. R408 で Windows が今までどおり起動するか(ドライバーを入れる前)。
  2. switchdisplay 1.0.2 を入れたあと、`Status.cmd` の `POST ownership:` に 720x1280 が入るか。0 のままなら、ドライバーは起動をやめ、Basic Display のまま。
- 戻し方: R405 で起動する。

ROM: `coreboot-R408.rom`、SHA-256 `e81331fe521cd99b67f183c0ee5ac1e6fe0e908d4bbd21f41292ab7f52e9143c`。作り方は `Tools/uefi/package-R408.sh`。

## 実機の結果

- 2026-11-08: Windows は今までどおり起動する。switchdisplay 1.0.2 が POST(720x1280、0xDFB80000)を受け取れた。1.0.10 でデスクトップの表示と回転(設定・SwitchCenter)ができた。
