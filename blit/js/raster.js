// Turn any drawable (image, canvas, video, ImageBitmap) into packed pixels in
// one of the protocol's formats (../PROTOCOL.md#pixel-formats): rows top-down,
// padded to a byte, leftmost pixel in the high bits; grey levels count ink
// (0 = white).

import { FORMAT, FORMAT_INFO, rowBytes } from './protocol.js';

/** Intrinsic size of a CanvasImageSource. */
export function sourceSize(src) {
  if (typeof HTMLVideoElement !== 'undefined' && src instanceof HTMLVideoElement) return { width: src.videoWidth, height: src.videoHeight };
  if (typeof HTMLImageElement !== 'undefined' && src instanceof HTMLImageElement) return { width: src.naturalWidth, height: src.naturalHeight };
  if (typeof VideoFrame !== 'undefined' && src instanceof VideoFrame) return { width: src.displayWidth, height: src.displayHeight };
  return { width: src.width, height: src.height };
}

/**
 * Where `fit` puts a sw × sh source in a width × height frame:
 * { dx, dy, dw, dh } in frame pixels. Hosts use it to map pointer events back.
 *
 * fit: 'cover' (fill, centre-crop) | 'contain' (letterbox on white) |
 *      'none' (native size, top-left, like the X4) | 'stretch'
 */
export function fitRect(sw, sh, width, height, fit) {
  if (fit === 'stretch') return { dx: 0, dy: 0, dw: width, dh: height };
  if (fit === 'none') return { dx: 0, dy: 0, dw: sw, dh: sh };
  const scale = fit === 'contain' ? Math.min(width / sw, height / sh) : Math.max(width / sw, height / sh);
  const dw = sw * scale;
  const dh = sh * scale;
  return { dx: (width - dw) / 2, dy: (height - dh) / 2, dw, dh };
}

/**
 * Rasterize `source` to width × height in `format` (default mono1).
 *
 * dither: 'floyd' (Floyd–Steinberg) | 'atkinson' | 'threshold' (none).
 * threshold (mono1 only), contrast, invert.
 *
 * Returns { pixels: Uint8Array, width, height, format, preview: ImageData }.
 */
export function rasterize(source, opts = {}) {
  const { width, height, fit = 'cover' } = opts;
  const canvas = new OffscreenCanvas(width, height);
  const ctx = canvas.getContext('2d', { willReadFrequently: true });
  ctx.fillStyle = '#fff';
  ctx.fillRect(0, 0, width, height);
  const { width: sw, height: sh } = sourceSize(source);
  if (sw && sh) {
    const { dx, dy, dw, dh } = fitRect(sw, sh, width, height, fit);
    ctx.imageSmoothingQuality = 'high';
    ctx.drawImage(source, dx, dy, dw, dh);
  }
  return rasterizeRgba(ctx.getImageData(0, 0, width, height), opts);
}

/**
 * The DOM-free half of rasterize(): RGBA pixels ({ data, width, height }, as
 * ImageData) -> packed pixels. `preview` is omitted when ImageData doesn't exist.
 */
export function rasterizeRgba({ data, width, height }, { format = FORMAT.MONO1, dither = 'floyd', threshold = 128, contrast = 1, invert = false } = {}) {
  const info = FORMAT_INFO[format];
  if (!info) throw new Error(`Unsupported format ${format}`);
  const adjust = (y) => {
    if (contrast !== 1) y = (y - 128) * contrast + 128;
    return invert ? 255 - y : y;
  };

  if (!info.levels) {
    // Colour: composite on white, adjust per channel, pack. No dithering.
    const rgb = new Uint8ClampedArray(width * height * 3);
    for (let i = 0, p = 0, q = 0; i < width * height; i++, p += 4, q += 3) {
      const a = data[p + 3] / 255;
      for (let c = 0; c < 3; c++) rgb[q + c] = adjust(data[p + c] * a + 255 * (1 - a));
    }
    return withPreview({ pixels: packRgb(rgb, width, height, format), width, height, format }, () => rgbPreview(rgb, width, height));
  }

  const gray = new Float32Array(width * height);
  for (let i = 0, p = 0; i < gray.length; i++, p += 4) {
    // Composite any transparency onto white, then Rec. 709 luma.
    const a = data[p + 3] / 255;
    gray[i] = adjust((0.2126 * data[p] + 0.7152 * data[p + 1] + 0.0722 * data[p + 2]) * a + 255 * (1 - a));
  }
  const ink = quantize(gray, width, height, info.levels, dither, threshold);
  return withPreview({ pixels: packLevels(ink, width, height, info.bpp), width, height, format }, () => inkPreview(ink, width, height, info.levels));
}

function withPreview(frame, make) {
  if (typeof ImageData !== 'undefined') frame.preview = make();
  return frame;
}

const KERNELS = {
  floyd: [
    [1, 0, 7 / 16],
    [-1, 1, 3 / 16],
    [0, 1, 5 / 16],
    [1, 1, 1 / 16],
  ],
  // Atkinson spreads only 3/4 of the error: crisper, higher-contrast results.
  atkinson: [
    [1, 0, 1 / 8],
    [2, 0, 1 / 8],
    [-1, 1, 1 / 8],
    [0, 1, 1 / 8],
    [1, 1, 1 / 8],
    [0, 2, 1 / 8],
  ],
};

/**
 * Grey (0–255, 255 = white; modified in place) -> ink levels (0 = white ..
 * levels-1 = black), with error diffusion unless dither is 'threshold'.
 */
export function quantize(gray, width, height, levels, dither = 'floyd', threshold = 128) {
  const out = new Uint8Array(gray.length);
  const top = levels - 1;
  const step = 255 / top;
  // For 1 bit, `threshold` sets the cut; for more levels, round to nearest.
  const level = levels === 2 ? (g) => (g < threshold ? 0 : 1) : (g) => Math.max(0, Math.min(top, Math.round(g / step)));
  const kernel = KERNELS[dither];
  for (let y = 0; y < height; y++) {
    for (let x = 0; x < width; x++) {
      const i = y * width + x;
      const old = gray[i];
      const q = level(old);
      out[i] = top - q;
      if (!kernel) continue;
      const err = old - q * step;
      for (const [dx, dy, f] of kernel) {
        const nx = x + dx;
        const ny = y + dy;
        if (nx >= 0 && nx < width && ny < height) gray[ny * width + nx] += err * f;
      }
    }
  }
  return out;
}

/** Pack one value per pixel (< 2^bpp) at 1, 2, 4 or 8 bpp. */
export function packLevels(values, width, height, bpp) {
  const stride = Math.ceil((width * bpp) / 8);
  const out = new Uint8Array(stride * height);
  const perByte = 8 / bpp;
  for (let y = 0; y < height; y++) {
    const row = y * stride;
    for (let x = 0; x < width; x++) {
      const v = values[y * width + x];
      if (v) out[row + Math.floor(x / perByte)] |= v << (8 - bpp - (x % perByte) * bpp);
    }
  }
  return out;
}

/** Unpack to one value per pixel (for previews and the simulated display). */
export function unpackLevels(bytes, width, height, bpp) {
  const stride = Math.ceil((width * bpp) / 8);
  const out = new Uint8Array(width * height);
  const perByte = 8 / bpp;
  const mask = (1 << bpp) - 1;
  for (let y = 0; y < height; y++) {
    for (let x = 0; x < width; x++) {
      out[y * width + x] = (bytes[y * stride + Math.floor(x / perByte)] >> (8 - bpp - (x % perByte) * bpp)) & mask;
    }
  }
  return out;
}

function packRgb(rgb, width, height, format) {
  const stride = rowBytes(format, width);
  const out = new Uint8Array(stride * height);
  for (let y = 0; y < height; y++) {
    for (let x = 0; x < width; x++) {
      const q = (y * width + x) * 3;
      if (format === FORMAT.RGB888) {
        out.set(rgb.subarray(q, q + 3), y * stride + x * 3);
      } else {
        const v = ((rgb[q] >> 3) << 11) | ((rgb[q + 1] >> 2) << 5) | (rgb[q + 2] >> 3);
        out[y * stride + x * 2] = v >> 8; // high byte first
        out[y * stride + x * 2 + 1] = v & 0xff;
      }
    }
  }
  return out;
}

function inkPreview(ink, width, height, levels) {
  const img = new ImageData(width, height);
  const top = levels - 1;
  for (let i = 0, p = 0; i < ink.length; i++, p += 4) {
    const v = Math.round(255 * (1 - ink[i] / top));
    img.data[p] = img.data[p + 1] = img.data[p + 2] = v;
    img.data[p + 3] = 255;
  }
  return img;
}

function rgbPreview(rgb, width, height) {
  const img = new ImageData(width, height);
  for (let i = 0, p = 0, q = 0; i < width * height; i++, p += 4, q += 3) {
    img.data[p] = rgb[q];
    img.data[p + 1] = rgb[q + 1];
    img.data[p + 2] = rgb[q + 2];
    img.data[p + 3] = 255;
  }
  return img;
}
