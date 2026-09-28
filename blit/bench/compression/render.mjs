// Screenshot the test pages at the X4's frame area (716 × 480), then turn every
// out/src/*.rgba into packed pixels with the host library's own rasterizer.
import { readFileSync, writeFileSync, readdirSync, mkdirSync } from 'node:fs';
import { createRequire } from 'node:module';
import { execSync } from 'node:child_process';
import { rasterizeRgba } from '../../js/raster.js';
import { FORMAT } from '../../js/protocol.js';

const step = process.argv[2];
if (step === 'shots') {
  // Playwright from the global install (this is a one-off tool, not a package).
  const root = execSync('npm root -g').toString().trim();
  const { chromium } = createRequire(import.meta.url)(`${root}/playwright`);
  const browser = await chromium.launch();
  const page = await browser.newPage({ viewport: { width: 716, height: 480 } });
  const pages = [['clock', 'clock.html'], ['clock2', 'clock.html?t=12:35'], ['dashboard', 'dashboard.html'],
    ['dashboard2', 'dashboard2.html'], ['article', 'article.html']];
  for (const [name, file] of pages) {
    await page.goto(new URL(`pages/${file}`, import.meta.url).href);
    await page.screenshot({ path: `out/src/${name}.png`, clip: { x: 0, y: 0, width: 716, height: 480 } });
  }
  await browser.close();
} else if (step === 'raster') {
  mkdirSync('out/bin', { recursive: true });
  const variants = [['mono1', FORMAT.MONO1, 'threshold'], ['mono1', FORMAT.MONO1, 'atkinson'],
    ['gray2', FORMAT.GRAY2, 'threshold'], ['gray2', FORMAT.GRAY2, 'atkinson'], ['rgb565', FORMAT.RGB565, 'threshold']];
  for (const f of readdirSync('out/src').filter((f) => f.endsWith('.rgba'))) {
    const data = new Uint8ClampedArray(readFileSync(`out/src/${f}`));
    for (const [fname, format, dither] of variants) {
      const { pixels } = rasterizeRgba({ data: data.slice(), width: 716, height: 480 }, { format, dither });
      writeFileSync(`out/bin/${f.slice(0, -5)}.${fname}.${dither}.bin`, pixels);
    }
  }
}
