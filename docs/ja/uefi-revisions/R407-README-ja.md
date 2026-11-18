# R407: 画面のデバイス DISP(試験)

R405 に、DSDT のデバイス `DISP`(`ACPI\SWOA0E01`)を 1 つ足しただけの版。ほかの動きは R405 と同じ。

- 使うドライバー: `switchdisplay.sys` 1.0.2 以降(WDDM の表示専用ドライバー。試験キット `SwitchDisplay-1.0.2.0-testkit`)。
- `_CRS`:
  - フレームバッファー `0xDFB80000`、`0x480000`(UEFI の「Display Reserved」、GOP)。最初に置く。Windows は、起動時の画面(POST)を、このアドレスをリソースに持つデバイスに渡す見込み。
  - DC-A `0x54200000`、`0x40000`
  - DSI-A `0x54300000`、`0x40000`
  - 割り込みはまだ無い(vblank は段階 3 で足す)。
- 経緯: ドライバー 1.0.0 と 1.0.1 はルートに置く仮のデバイスを使った。1.0.0 は起動時の画面を受け取れず(大きさがすべて 0)、1.0.1 は INF の LogConfig でこの範囲を割り当てようとしたが、エラー 12(アドレスの衝突)になった。
- 戻し方: R405 で起動すれば `DISP` が無くなり、switchdisplay は読み込まれない(Windows は Basic Display に戻る)。

ROM: `coreboot-R407.rom`、SHA-256 `3f0ef7c1d3d1bf2fdf289866cb902c5ce9c41f5f77d2f9efcd987c0d8069980e`。作り方は `Tools/uefi/package-R407.sh`。

## 実機の結果

- 2026-11-08: **起動しない。** Switch の画面が細かいノイズの模様になり、Windows が起動しない。switchdisplay を入れる前(R407 で起動した最初)に起きた。R405 では起動する。
- 見立て(未確認): FB の範囲がデバイスのリソースになったことで、Windows がこのメモリーの扱いを変えた(Basic Display が付かない、またはこのメモリーを別の用途に使った)。
- **R407 は使わない。** 次の試し: FB を `_CRS` から外し、DC-A と DSI-A だけにした版。
