// Build the extension into a folder Chrome can load.
//
//   node build.mjs            build/dev: unbundled ES modules, for development
//   node build.mjs --watch    the same, rebuilt whenever a source file changes
//   node build.mjs --dist     build/dist: bundled and minified, plus build/blit-<version>.zip
//
// The sources import the protocol library as `./lib/*.js`. That folder isn't
// in the source tree: the library lives in ../js and is shared with the web
// demo. The dev build copies ../js into build/dev/lib/ so the modules load
// as they are, one file each, easy to step through in DevTools. The dist
// build resolves ./lib/ to ../js and bundles each entry point into a single
// file, with no lib/ folder at all.

import { createHash } from 'node:crypto';
import { cpSync, existsSync, mkdirSync, readFileSync, readdirSync, rmSync, statSync, watch, writeFileSync } from 'node:fs';
import { dirname, join, relative, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { deflateRawSync } from 'node:zlib';

const here = dirname(fileURLToPath(import.meta.url));
const libSrc = resolve(here, '../js');
const args = new Set(process.argv.slice(2));
const dist = args.has('--dist');
const out = join(here, 'build', dist ? 'dist' : 'dev');

// Loaded by Chrome directly (manifest and HTML), copied as they are.
const STATIC = ['manifest.json', 'popup.html', 'session.html', 'style.css', 'icons'];
// ES modules loaded by <script type="module">: bundled for dist.
const MODULES = ['popup.js', 'session.js'];
// Classic scripts (content script, worker): kept standalone in both builds.
const SCRIPTS = ['content.js', 'ticker.js'];
// Imported by the modules; bundled into them for dist.
const SHARED = ['config.js'];
// The library files the extension uses, from ../js.
const LIB = ['blit.js', 'protocol.js', 'raster.js', 'dom-capture.js', 'sim-display.js'];

function copy(from, to) {
  mkdirSync(dirname(to), { recursive: true });
  cpSync(from, to, { recursive: true });
}

function buildDev() {
  rmSync(out, { recursive: true, force: true });
  for (const f of [...STATIC, ...MODULES, ...SCRIPTS, ...SHARED]) copy(join(here, f), join(out, f));
  for (const f of LIB) copy(join(libSrc, f), join(out, 'lib', f));
}

async function buildDist() {
  const esbuild = await import('esbuild').catch(() => {
    throw new Error('esbuild is missing: run `npm install` in blit/chrome-extension first');
  });
  rmSync(out, { recursive: true, force: true });
  for (const f of STATIC) copy(join(here, f), join(out, f));

  // ./lib/x.js -> ../js/x.js, so the bundle takes the library from its source.
  const libAlias = {
    name: 'lib-alias',
    setup(build) {
      build.onResolve({ filter: /^\.\/lib\// }, (a) => ({ path: join(libSrc, a.path.slice('./lib/'.length)) }));
    },
  };
  const common = { bundle: true, minify: true, target: 'chrome116', legalComments: 'none', logLevel: 'warning' };
  await esbuild.build({
    ...common,
    entryPoints: MODULES.map((f) => join(here, f)),
    outdir: out,
    format: 'esm', // top-level await, and loaded as type="module"
    plugins: [libAlias],
  });
  await esbuild.build({ ...common, entryPoints: SCRIPTS.map((f) => join(here, f)), outdir: out, format: 'iife' });

  const { version } = JSON.parse(readFileSync(join(here, 'manifest.json'), 'utf8'));
  const zipPath = join(here, 'build', `blit-${version}.zip`);
  writeFileSync(zipPath, zip(out));
  return zipPath;
}

// Every file the manifest and HTML pages point at, and every relative
// import, must exist in the output: a missing one only shows up in Chrome.
function verify() {
  const missing = [];
  const need = (from, ref) => {
    const path = resolve(dirname(from), ref);
    if (!existsSync(path)) missing.push(`${relative(out, from)} -> ${ref}`);
  };
  const manifest = JSON.parse(readFileSync(join(out, 'manifest.json'), 'utf8'));
  const manifestFile = join(out, 'manifest.json');
  for (const icon of Object.values({ ...manifest.icons, ...manifest.action?.default_icon })) need(manifestFile, icon);
  if (manifest.action?.default_popup) need(manifestFile, manifest.action.default_popup);
  for (const file of walk(out)) {
    const text = file.endsWith('.html') || file.endsWith('.js') ? readFileSync(file, 'utf8') : '';
    if (file.endsWith('.html')) {
      for (const [, ref] of text.matchAll(/(?:src|href)="([^":#]+)"/g)) need(file, ref);
    } else if (file.endsWith('.js')) {
      for (const [, ref] of text.matchAll(/(?:from\s*|import\s*\(\s*)["'](\.{1,2}\/[^"']+)["']/g)) need(file, ref);
      // Files the code loads by name: the worker and the injected content script.
      for (const [, ref] of text.matchAll(/(?:new Worker\(|files:\s*\[)\s*["']([^"']+)["']/g)) need(join(out, 'x'), ref);
    }
  }
  if (missing.length) throw new Error(`Missing from the build:\n  ${missing.join('\n  ')}`);
}

function* walk(dir) {
  for (const name of readdirSync(dir).sort()) {
    const path = join(dir, name);
    if (statSync(path).isDirectory()) yield* walk(path);
    else yield path;
  }
}

// A minimal zip writer (deflate, no dependencies), for the Web Store upload.
function zip(dir) {
  const crcTable = Array.from({ length: 256 }, (_, n) => {
    let c = n;
    for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
    return c >>> 0;
  });
  const crc32 = (buf) => {
    let c = ~0;
    for (const b of buf) c = crcTable[(c ^ b) & 0xff] ^ (c >>> 8);
    return ~c >>> 0;
  };
  const local = [];
  const central = [];
  let offset = 0;
  for (const path of walk(dir)) {
    const name = Buffer.from(relative(dir, path).split('\\').join('/'));
    const data = readFileSync(path);
    const packed = deflateRawSync(data, { level: 9 });
    const crc = crc32(data);
    // Fixed timestamp (1980-01-01) so the same sources make the same zip.
    const fields = (sig, extra) => {
      const b = Buffer.alloc(extra ? 46 : 30);
      let i = 0;
      b.writeUInt32LE(sig, i); i += 4;
      if (extra) { b.writeUInt16LE(20, i); i += 2; } // version made by
      b.writeUInt16LE(20, i); i += 2; // version needed
      b.writeUInt16LE(0x0800, i); i += 2; // UTF-8 names
      b.writeUInt16LE(8, i); i += 2; // deflate
      b.writeUInt16LE(0, i); i += 2; // time
      b.writeUInt16LE(0x21, i); i += 2; // date
      b.writeUInt32LE(crc, i); i += 4;
      b.writeUInt32LE(packed.length, i); i += 4;
      b.writeUInt32LE(data.length, i); i += 4;
      b.writeUInt16LE(name.length, i); i += 2;
      b.writeUInt16LE(0, i); i += 2; // extra field
      if (extra) b.writeUInt32LE(offset, 42); // local header offset (other fields 0)
      return b;
    };
    const header = fields(0x04034b50, false);
    local.push(header, name, packed);
    central.push(fields(0x02014b50, true), name);
    offset += header.length + name.length + packed.length;
  }
  const dirBuf = Buffer.concat(central);
  const count = central.length / 2;
  const end = Buffer.alloc(22);
  end.writeUInt32LE(0x06054b50, 0);
  end.writeUInt16LE(count, 8);
  end.writeUInt16LE(count, 10);
  end.writeUInt32LE(dirBuf.length, 12);
  end.writeUInt32LE(offset, 16);
  return Buffer.concat([...local, dirBuf, end]);
}

function summary() {
  let bytes = 0;
  let files = 0;
  const hash = createHash('sha256');
  for (const f of walk(out)) {
    const data = readFileSync(f);
    bytes += data.length;
    files++;
    hash.update(relative(out, f)).update(data);
  }
  return `${files} files, ${(bytes / 1024).toFixed(1)} KiB, sha256 ${hash.digest('hex').slice(0, 12)}`;
}

async function build() {
  const zipPath = dist ? await buildDist() : buildDev();
  verify();
  console.log(`Built ${relative(process.cwd(), out) || '.'} (${dist ? 'bundled' : 'unbundled'}): ${summary()}`);
  if (zipPath) console.log(`Packed ${relative(process.cwd(), zipPath)}`);
}

await build();

if (args.has('--watch')) {
  if (dist) throw new Error('--watch is for the dev build');
  console.log('Watching for changes (Ctrl-C to stop). Reload the extension in chrome://extensions after a rebuild.');
  let timer = null;
  const rebuild = (_event, file) => {
    if (file && /(^|[/\\])(build|node_modules)([/\\]|$)/.test(file)) return;
    clearTimeout(timer);
    timer = setTimeout(() => build().catch((err) => console.error(err.message)), 100);
  };
  watch(here, { recursive: true }, rebuild);
  watch(libSrc, { recursive: true }, rebuild);
}
