// PNG in and out: Chromium's screenshots in, previews and the simulator's
// panel out.

import { writeFile } from 'node:fs/promises';

import { PNG } from 'pngjs';

import { FORMAT_INFO } from '../../js/protocol.js';
import { unpackLevels } from '../../js/raster.js';

export function decodePng(bytes: Uint8Array): { data: Uint8ClampedArray; width: number; height: number } {
  const png = PNG.sync.read(Buffer.from(bytes));
  return { data: new Uint8ClampedArray(png.data.buffer, png.data.byteOffset, png.data.length), width: png.width, height: png.height };
}

export async function writeRgbaPng(path: string, data: Uint8Array | Uint8ClampedArray, width: number, height: number): Promise<void> {
  const png = new PNG({ width, height });
  png.data = Buffer.from(data.buffer, data.byteOffset, data.byteLength);
  await writeFile(path, PNG.sync.write(png));
}

/** Packed grey pixels (as sent to the display) -> a PNG of what the display shows. */
export async function writeFramePng(path: string, pixels: Uint8Array, format: number, width: number, height: number): Promise<void> {
  const info = FORMAT_INFO[format as keyof typeof FORMAT_INFO] as { bpp: number; levels?: number };
  const rgba = new Uint8Array(width * height * 4);
  if (info.levels) {
    const ink = unpackLevels(pixels, width, height, info.bpp);
    const top = info.levels - 1;
    for (let i = 0; i < ink.length; i++) {
      rgba.fill(Math.round(255 * (1 - ink[i] / top)), i * 4, i * 4 + 3);
      rgba[i * 4 + 3] = 255;
    }
  } else {
    // Colour: rgb888, or rgb565 high byte first.
    const stride = Math.ceil((width * info.bpp) / 8);
    for (let y = 0; y < height; y++) {
      for (let x = 0; x < width; x++) {
        const o = (y * width + x) * 4;
        if (info.bpp === 24) {
          rgba.set(pixels.subarray(y * stride + x * 3, y * stride + x * 3 + 3), o);
        } else {
          const v = (pixels[y * stride + x * 2] << 8) | pixels[y * stride + x * 2 + 1];
          rgba[o] = ((v >> 11) & 31) * 255 / 31;
          rgba[o + 1] = ((v >> 5) & 63) * 255 / 63;
          rgba[o + 2] = (v & 31) * 255 / 31;
        }
        rgba[o + 3] = 255;
      }
    }
  }
  await writeRgbaPng(path, rgba, width, height);
}
