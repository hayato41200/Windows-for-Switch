# Mac から Switch を起動する（RCM で coreboot + UEFI を送る）

Windows の PC では、WSL の `shofel2.py` で送っていた。Mac でも、同じ `shofel2.py` がそのまま使える（macOS 用の処理が元から入っている）。

## 中身

| ファイル | 内容 |
|---|---|
| `boot-mac.sh` | 確認してから送る。ROM のファイルを引数で渡す |
| `shofel2.py` | 送信用のスクリプト（`work\shofel2\exploit` と同じもの） |
| `cbfs.bin` | 最初に送る小さな読み込み用プログラム（同上） |
| `SHA256SUMS.txt` | 上の2つのハッシュ |

ROM（`coreboot-R3xx.rom`）は入れていない。使う版を、PC の `D:\Codex\` から Mac にコピーする。新しい版を作ったら、その都度コピーする。

## 準備（最初の1回だけ）

ターミナルで次を実行する。

1. **Homebrew が無ければ入れる。** https://brew.sh の1行を実行する。
2. **libusb と Python を入れる。**
   ```bash
   brew install libusb python
   ```
3. **専用の Python 環境を作って、pyusb を入れる。**
   ```bash
   "$(brew --prefix)/bin/python3" -m venv ~/swoa-rcm-venv
   ~/swoa-rcm-venv/bin/pip install pyusb
   ```
   - macOS の `/usr/bin/python3` ではなく、Homebrew の Python を使う。macOS の保護機能（SIP）のため、`/usr/bin/python3` だと、Homebrew の libusb を見つけられないことがある。
4. **このフォルダーを Mac にコピーする。** 例: `~/SwitchWOA/Mac-RCM-Boot`。ROM も同じフォルダーに置くと楽。

## 起動のしかた

1. **Switch を RCM にする。** いつもどおり、ジグを付けて、音量＋を押しながら電源を入れる。
2. **USB-C のケーブルで、Mac に直接つなぐ。** ハブや変換アダプターは、できるだけ通さない。
3. **ターミナルで送る。**
   ```bash
   cd ~/SwitchWOA/Mac-RCM-Boot
   bash boot-mac.sh coreboot-R375.rom
   ```
   - `BOOT` と入力すると送信が始まる。
   - `timeout.. good!` のあとに `sending 0x... bytes @0x...` が続き、最後に `you have been served` と出れば、送信は完了。
4. **Switch の画面に UEFI の表示が出るのを待つ。** その後は、いつもどおり Windows が起動する。

## うまくいかないとき

- **「RCM 中の Switch (0955:7321) が見つかりません」:**
  - Switch が RCM になっていない。画面が真っ暗のまま、で正しい状態。
  - ケーブルが充電専用である可能性もある。
  - 「システム情報」の USB の一覧に「APX」が出ていれば、Mac からは見えている。
- **`No backend available`:** libusb が見つからない。`brew install libusb` を確かめる。Intel の Mac なら `/usr/local/lib`、Apple シリコンなら `/opt/homebrew/lib` に `libusb-1.0.dylib` があるはず。
- **`Performing hax...` のあとで止まる、または送信が途中で止まる:** Switch の電源を長押しで切り、RCM からやり直す。それでも同じなら、その画面を見せてほしい。
  - **[未確認]** Mac での送信は、まだ試したことがない。Mac の機種（Apple シリコンか Intel か）と macOS の版によって動きが変わる可能性がある。

## ROM を Mac に置くときの注意

- ROM は約 10 MB。コピーの方法は、AirDrop、共有フォルダー、USB メモリなど何でもよい。
- 中身が正しいか確かめたいときは、`shasum -a 256 coreboot-R375.rom` の値を、各版の README の SHA-256 と比べる。例えば R375 は `5320e758…` で始まる。
