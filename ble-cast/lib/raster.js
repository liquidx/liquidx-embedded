// Turn any drawable (image, canvas, video, ImageBitmap) into a 1-bit frame in
// the protocol's raw1 format: rows top-down, MSB = leftmost pixel, 1 = black.

/** Intrinsic size of a CanvasImageSource. */
export function sourceSize(src) {
  if (src instanceof HTMLVideoElement) return { width: src.videoWidth, height: src.videoHeight };
  if (src instanceof HTMLImageElement) return { width: src.naturalWidth, height: src.naturalHeight };
  return { width: src.width, height: src.height };
}

/**
 * Rasterize `source` to width × height and dither it to 1 bit.
 *
 * fit: 'cover' (fill, centre-crop) | 'contain' (letterbox on white) |
 *      'none' (native size, top-left, like the device) | 'stretch'
 * dither: 'floyd' (Floyd–Steinberg) | 'atkinson' | 'threshold'
 *
 * Returns { bits: Uint8Array, width, height, preview: ImageData }.
 */
export function rasterize(source, { width, height, fit = 'cover', dither = 'floyd', threshold = 128, contrast = 1, invert = false } = {}) {
  const canvas = new OffscreenCanvas(width, height);
  const ctx = canvas.getContext('2d', { willReadFrequently: true });
  ctx.fillStyle = '#fff';
  ctx.fillRect(0, 0, width, height);
  drawFitted(ctx, source, width, height, fit);

  const { data } = ctx.getImageData(0, 0, width, height);
  const gray = new Float32Array(width * height);
  for (let i = 0, p = 0; i < gray.length; i++, p += 4) {
    // Composite any transparency onto white, then Rec. 709 luma.
    const a = data[p + 3] / 255;
    let y = (0.2126 * data[p] + 0.7152 * data[p + 1] + 0.0722 * data[p + 2]) * a + 255 * (1 - a);
    if (contrast !== 1) y = (y - 128) * contrast + 128;
    gray[i] = invert ? 255 - y : y;
  }

  const black = DITHERS[dither]?.(gray, width, height, threshold) ?? thresholdOnly(gray, threshold);
  return { bits: pack(black, width, height), width, height, preview: previewOf(black, width, height) };
}

function drawFitted(ctx, source, width, height, fit) {
  const { width: sw, height: sh } = sourceSize(source);
  if (!sw || !sh) return;
  if (fit === 'stretch') return ctx.drawImage(source, 0, 0, width, height);
  if (fit === 'none') return ctx.drawImage(source, 0, 0);
  const scale = fit === 'contain' ? Math.min(width / sw, height / sh) : Math.max(width / sw, height / sh);
  const dw = sw * scale;
  const dh = sh * scale;
  ctx.imageSmoothingQuality = 'high';
  ctx.drawImage(source, (width - dw) / 2, (height - dh) / 2, dw, dh);
}

// Each returns a Uint8Array of 0/1 (1 = black).
const DITHERS = {
  threshold: (g, w, h, t) => thresholdOnly(g, t),
  floyd: (g, w, h, t) =>
    diffuse(g, w, h, t, [
      [1, 0, 7 / 16],
      [-1, 1, 3 / 16],
      [0, 1, 5 / 16],
      [1, 1, 1 / 16],
    ]),
  // Atkinson spreads only 3/4 of the error: crisper, higher-contrast results.
  atkinson: (g, w, h, t) =>
    diffuse(g, w, h, t, [
      [1, 0, 1 / 8],
      [2, 0, 1 / 8],
      [-1, 1, 1 / 8],
      [0, 1, 1 / 8],
      [1, 1, 1 / 8],
      [0, 2, 1 / 8],
    ]),
};

function thresholdOnly(g, t) {
  const out = new Uint8Array(g.length);
  for (let i = 0; i < g.length; i++) out[i] = g[i] < t ? 1 : 0;
  return out;
}

function diffuse(g, w, h, t, kernel) {
  const out = new Uint8Array(g.length);
  for (let y = 0; y < h; y++) {
    for (let x = 0; x < w; x++) {
      const i = y * w + x;
      const old = g[i];
      const isBlack = old < t;
      out[i] = isBlack ? 1 : 0;
      const err = old - (isBlack ? 0 : 255);
      for (const [dx, dy, f] of kernel) {
        const nx = x + dx;
        const ny = y + dy;
        if (nx >= 0 && nx < w && ny < h) g[ny * w + nx] += err * f;
      }
    }
  }
  return out;
}

function pack(black, width, height) {
  const stride = Math.ceil(width / 8);
  const bits = new Uint8Array(stride * height);
  for (let y = 0; y < height; y++) {
    for (let x = 0; x < width; x++) {
      if (black[y * width + x]) bits[y * stride + (x >> 3)] |= 0x80 >> (x & 7);
    }
  }
  return bits;
}

function previewOf(black, width, height) {
  const img = new ImageData(width, height);
  for (let i = 0, p = 0; i < black.length; i++, p += 4) {
    const v = black[i] ? 0 : 255;
    img.data[p] = img.data[p + 1] = img.data[p + 2] = v;
    img.data[p + 3] = 255;
  }
  return img;
}
