# R402・R403 — Windows の音声（R401 ベース）

Windows のドライバー `switchaudio.sys`（[Windows-for-Switch-Drivers](https://github.com/hayato41200/Windows-for-Switch-Drivers) の `drivers/SwitchAudio`）で音を出すための UEFI の変更。

## R402: スピーカー

- 起動時に、音の経路を準備したまま Windows に引き渡す。
  - APE のクロック、ADMA、ADMAIF、XBAR、I2S1、コーデック ALC5639。
  - 経路は R393〜R401 で確かめたもの（`R401-README-ja.md`）。
- スピーカーの音量を、コーデックで -18 dB（SPK_VOL 0x1414）にする。
- DSDT に音声のデバイス AUD0（`ACPI\SWOA0C01`）を加える。
- ドライバーは、APE のクロックが入っていないと音声のレジスタに触らずに止まる。クロックが止まった状態で読むと、CPU が止まるため。

## R403: ヘッドホン

- コーデックのヘッドホンの経路を用意する。
  - 経路: DAC1 → OUT MIX → HPOVOL → HPO MIX → HP アンプ。
  - HP アンプの電源の入れ方とポップ音の対策は、Linux の rt5640 ドライバー（`hp_amp_power_on`、`pmu_depop`）と同じ手順。
  - ヘッドホンの音量は -24 dB（HP_VOL 0x1818）。
- ジャックのスイッチは GPIO の **PV6**（パッドは TOUCH_RST、Linux の `realtek,jack-detect-gpios`）。差すと 0 になる。
  - パッド（PINMUX 0x70003214）を入力にし、GPIO の入力として使う。
- 起動したときにヘッドホンが差さっていれば、スピーカーを消音してヘッドホンを鳴らす。差さっていなければ、その逆。
- 起動画面に、次のような行が出る: `R403 audio: headphone path ok, PV6 pinmux=... in=1: speaker`
- Windows に入ったあとは、`switchaudio.sys` 1.0.0.7 以降が PV6 を 0.2 秒ごとに見て、I2C でコーデックの消音を切り替える。

## 実機での結果（2026-11-02、ユーザー報告）

- R402 + `switchaudio.sys` 1.0.0.6: Windows の音がスピーカーから出た。音飛びも無い。
- R403 + `switchaudio.sys` 1.0.0.7: ヘッドホンから音が出た。抜き差しで出力が切り替わった（1回目の試験で動いた）。

## ビルド

`Tools/uefi/package-R403.sh`（パスは元の環境のもの）。
