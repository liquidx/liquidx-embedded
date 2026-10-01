#!/bin/sh
# Render the firmware's screens on this computer, to docs/screens/*.png, plus
# a contact sheet at 3x (docs/screens/all.png). Needs SDL2 and Pillow.
set -e
cd "$(dirname "$0")/.."
OUT=${1:-docs/screens}
TMP=.pio/sim-out
pio run -e sim -s
rm -rf "$TMP" && mkdir -p "$TMP" "$OUT"
.pio/build/sim/program "$TMP" > /dev/null
python3 - "$TMP" "$OUT" <<'PY'
import glob, os, sys
from PIL import Image

src, out = sys.argv[1:]
names = sorted(glob.glob(f"{src}/*.ppm"))
images = [Image.open(n).convert("RGB") for n in names]
for name, image in zip(names, images):
    image.save(f"{out}/{os.path.basename(name)[:-4]}.png")

scale, cols, gap = 3, 3, 12
w, h = images[0].width * scale, images[0].height * scale
rows = (len(images) + cols - 1) // cols
sheet = Image.new("RGB", (cols * (w + gap) + gap, rows * (h + gap) + gap), (60, 60, 66))
for i, image in enumerate(images):
    x, y = gap + (i % cols) * (w + gap), gap + (i // cols) * (h + gap)
    sheet.paste(image.resize((w, h), Image.NEAREST), (x, y))
sheet.save(f"{out}/all.png")
print(f"{len(images)} screens in {out}/")
PY
