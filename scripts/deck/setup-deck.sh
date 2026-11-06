#!/usr/bin/env bash
# Copyright (c) 2026 Akhil Moola (ThisIsAkill). MIT License, see LICENSE.
# First-time setup on the Steam Deck (or any Linux PC): extracts and patches the menu archive
# from your own retail ISO and records the ISO path. The ISO itself is only read.
#
#   ./setup-deck.sh /path/to/TableTennis.iso
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
iso="$(realpath -e "${1:?usage: ./setup-deck.sh /path/to/TableTennis.iso}")"
flash="$here/assets/resource/flash/flash.rpf"
mkdir -p "$(dirname "$flash")"
python3 -I - "$here/tools" "$iso" "$flash" <<'PY'
import sys
sys.path.insert(0, sys.argv[1])
from pathlib import Path
from disc_image import read_inputs
from patch_menu import patch
_, flash = read_inputs(sys.argv[2])
out = Path(sys.argv[3])
tmp = out.with_suffix('.tmp')
tmp.write_bytes(flash)
tmp.replace(out)
patch(str(out))
print('Menu archive extracted and patched; ISO unchanged.')
PY
cfg="$here/tt-recomp.ini"
if [[ -f "$cfg" ]]; then
  { echo "iso_path=$iso"; grep -v '^iso_path=' "$cfg" || true; } > "$cfg.tmp"
else
  printf 'iso_path=%s\nresolution=800p\nframerate=60\nfullscreen=true\nshow_fps=false\naa=none\npost=bilinear\naspect=letterbox\n' \
    "$iso" > "$cfg.tmp"
fi
mv -f "$cfg.tmp" "$cfg"
echo "Ready. Start the game with: $here/tt-recomp.sh"
