# Compression benchmark

Measures how small blit frames get with PackBits (encoding 1, what hosts and
the X4 use today) and the alternatives discussed in
[RESEARCH.md](../../RESEARCH.md#compression). Frames are rendered at the X4's
frame area (716 × 480) and converted with the host library's own rasterizer
(`js/raster.js`), so the bytes are the ones a host would send.

Inputs: a clock and a dashboard (each also with a minute's changes), a page of
text, two of the X4's own UI mockups, and two photos (from scikit-image).

```sh
pip install numpy pillow scikit-image heatshrink2 lz4 zstandard
npm install -g playwright   # uses the Chromium that Playwright finds
python3 bench.py            # prints Markdown tables; work files go in out/
```

Only PackBits is a real blit encoding today. "PackBits-up" is the proposal in
RESEARCH.md: each byte XORed with the byte above it, then PackBits with a
`0x80 u16` long zero run. zstd is there as a reference point, not a
candidate.
