#!/usr/bin/env bash
# Boot the Switch (RCM) with a coreboot+UEFI ROM from macOS.
# Usage: bash boot-mac.sh coreboot-R375.rom
set -euo pipefail
cd "$(dirname "$0")"

ROM="${1:-}"
if [[ -z "$ROM" || ! -f "$ROM" ]]; then
  echo "使い方: bash boot-mac.sh <coreboot-R3xx.rom>" >&2
  exit 1
fi
for f in shofel2.py cbfs.bin SHA256SUMS.txt; do
  [[ -f "$f" ]] || { echo "不足: $f" >&2; exit 1; }
done
shasum -a 256 -c SHA256SUMS.txt

VENV="$HOME/swoa-rcm-venv"
if [[ ! -x "$VENV/bin/python3" ]]; then
  echo "Python の環境がありません。README-ja.md の「準備」を先に行ってください。" >&2
  exit 1
fi
# pyusb needs Homebrew's libusb (Apple silicon: /opt/homebrew, Intel: /usr/local).
export DYLD_FALLBACK_LIBRARY_PATH="/opt/homebrew/lib:/usr/local/lib${DYLD_FALLBACK_LIBRARY_PATH:+:$DYLD_FALLBACK_LIBRARY_PATH}"

if ! ioreg -p IOUSB -l -w 0 | grep -q '"idProduct" = 29473'; then
  echo "RCM 中の Switch (0955:7321) が見つかりません。送信は行いません。" >&2
  echo "Switch を RCM にして、Mac に直接（ハブなしで）つないでください。" >&2
  exit 1
fi

echo "ROM: $ROM"
shasum -a 256 "$ROM"
echo "対象は未対策の初期型 Erista だけです。eMMC への書き込みはしません。"
read -r -p "起動する場合は BOOT と入力: " answer
[[ "$answer" == "BOOT" ]] || { echo "中止しました。"; exit 1; }

exec "$VENV/bin/python3" ./shofel2.py ./cbfs.bin "$ROM"
