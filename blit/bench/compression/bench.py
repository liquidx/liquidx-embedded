"""Compression benchmark for blit frames. See README.md."""
import glob, io, os, subprocess, sys, zlib
import heatshrink2, lz4.block, numpy as np, zstandard
from PIL import Image
from skimage import data as skdata

W, H = 716, 480
BPP = {'mono1': 1, 'gray2': 2, 'rgb565': 16}
HERE = os.path.dirname(os.path.abspath(__file__))
os.chdir(HERE)


def rowbytes(fmt):
    return (W * BPP[fmt] + 7) // 8


# --- codecs -----------------------------------------------------------------

def packbits(b):
    """Encoding 1, exactly as js/protocol.js packbits()."""
    out = bytearray(); i = 0; n = len(b)
    while i < n:
        run = 1
        while i + run < n and run < 128 and b[i + run] == b[i]: run += 1
        if run >= 3:
            out += bytes([257 - run, b[i]]); i += run; continue
        s = i
        while i < n and i - s < 128:
            if i + 2 < n and b[i] == b[i + 1] == b[i + 2]: break
            i += 1
        out.append(i - s - 1); out += b[s:i]
    return bytes(out)


def packbits_z(b, min_zeros=320):
    """PackBits plus `0x80 u16 n` = n + 1 zero bytes (0x80 is a no-op in PackBits)."""
    out = bytearray(); i = s = 0; n = len(b)
    while i < n:
        if b[i]:
            i += 1; continue
        j = i
        while j < n and b[j] == 0 and j - i < 65536: j += 1
        if j - i >= min_zeros:
            out += packbits(b[s:i]); k = j - i - 1; out += bytes([0x80, k & 255, k >> 8]); s = j
        i = j
    return bytes(out + packbits(b[s:]))


def up(b, rb):
    """XOR each byte with the byte one row above (PNG's Up filter, with XOR)."""
    a = np.frombuffer(b, np.uint8).reshape(-1, rb)
    f = a.copy(); f[1:] ^= a[:-1]
    return f.tobytes()


def deflate(b, wbits):
    c = zlib.compressobj(9, zlib.DEFLATED, -wbits, 9)
    return c.compress(b) + c.flush()


def g4(b):
    im = Image.frombytes('1', (W, H), bytes(255 - x for x in b))  # PIL: 0 = black
    f = io.BytesIO(); im.save(f, 'TIFF', compression='group4'); f.seek(0)
    return sum(Image.open(f).tag_v2[279])  # StripByteCounts


CODECS = {
    'PackBits (1)': lambda b, fmt: len(packbits(b)),
    'PackBits-up': lambda b, fmt: len(packbits_z(up(b, rowbytes(fmt)))),
    'heatshrink w11': lambda b, fmt: len(heatshrink2.compress(b, window_sz2=11, lookahead_sz2=4)),
    'LZ4 HC': lambda b, fmt: len(lz4.block.compress(b, mode='high_compression', compression=12, store_size=False)),
    'deflate 1 KB': lambda b, fmt: len(deflate(b, 10)),
    'deflate 32 KB': lambda b, fmt: len(deflate(b, 15)),
    'CCITT G4': lambda b, fmt: g4(b) if fmt == 'mono1' else None,
    'zstd 19': lambda b, fmt: len(zstandard.ZstdCompressor(level=19).compress(b)),
}


# --- inputs -----------------------------------------------------------------

def prepare():
    os.makedirs('out/src', exist_ok=True)
    subprocess.run(['node', 'render.mjs', 'shots'], check=True)

    def cover(im):
        im = im.convert('RGB'); s = max(W / im.width, H / im.height)
        im = im.resize((round(im.width * s), round(im.height * s)), Image.LANCZOS)
        l, t = (im.width - W) // 2, (im.height - H) // 2
        return im.crop((l, t, l + W, t + H))
    for name in ['astronaut', 'coffee']:
        cover(Image.fromarray(getattr(skdata, name)())).save(f'out/src/photo-{name}.png')
    for f in ['01-home', '03-settings']:  # the X4's own UI mockups, cropped to the frame area
        Image.open(f'../../../xteink-x4-platformio/docs/design/{f}.png').convert('RGB') \
            .crop((0, 0, W, H)).save(f'out/src/x4-{f[3:]}.png')
    for f in glob.glob('out/src/*.png'):
        open(f[:-4] + '.rgba', 'wb').write(Image.open(f).convert('RGBA').tobytes())
    subprocess.run(['node', 'render.mjs', 'raster'], check=True)


def frame(name, fmt, dither='threshold'):
    return open(f'out/bin/{name}.{fmt}.{dither}.bin', 'rb').read()


# --- reports ----------------------------------------------------------------

def table(head, rows):
    print('| ' + ' | '.join(head) + ' |')
    print('|' + '---|' * len(head))
    for r in rows: print('| ' + ' | '.join(str(x) for x in r) + ' |')
    print()


def full_frames():
    images = ['clock', 'x4-home', 'x4-settings', 'dashboard', 'article', 'photo-coffee', 'photo-astronaut']
    for fmt, dithers in [('mono1', ['threshold', 'atkinson']), ('gray2', ['threshold', 'atkinson']), ('rgb565', ['threshold'])]:
        print(f'### Full frames, {fmt}\n')
        codecs = [k for k in CODECS if not (k == 'CCITT G4' and fmt != 'mono1')]
        rows = []
        for img in images:
            for d in dithers:
                b = frame(img, fmt, d)
                rows.append([img, d, len(b)] + [CODECS[k](b, fmt) for k in codecs])
        table(['image', 'dither', 'raw'] + codecs, rows)


def deltas():
    print('### Updates (12:34 to 12:35, and four values changed on the dashboard)\n')
    rows = []
    for a, b, fmt in [('clock', 'clock2', 'mono1'), ('dashboard', 'dashboard2', 'mono1'), ('dashboard', 'dashboard2', 'gray2')]:
        old, new = frame(a, fmt), frame(b, fmt); rb = rowbytes(fmt)
        x = bytes(p ^ q for p, q in zip(old, new))
        arr = np.frombuffer(x, np.uint8).reshape(H, rb); ys, xs = np.nonzero(arr)
        box = np.frombuffer(new, np.uint8).reshape(H, rb)[ys.min():ys.max() + 1, xs.min():xs.max() + 1].tobytes()
        px = 8 // BPP[fmt] if BPP[fmt] < 8 else 1
        rows.append([f'{b} {fmt}', len(packbits(new)), len(deflate(new, 10)),
                     f'{(xs.max() + 1 - xs.min()) * px}×{ys.max() + 1 - ys.min()}', len(packbits(box)), len(deflate(box, 10)),
                     len(packbits(x)), len(packbits_z(up(x, rb))), len(deflate(x, 10))])
    table(['update', 'full PackBits', 'full deflate', 'region size', 'region PackBits', 'region deflate',
           'XOR PackBits', 'XOR PackBits-up', 'XOR deflate'], rows)


def windows():
    print('### Deflate window size\n')
    rows = []
    for img in ['clock', 'dashboard', 'article', 'photo-coffee']:
        for fmt in ['mono1', 'gray2', 'rgb565']:
            b = frame(img, fmt)
            rows.append([img, fmt] + [len(deflate(b, w)) for w in (9, 10, 12, 15)])
    table(['image', 'format', '512 B', '1 KB', '4 KB', '32 KB'], rows)


if __name__ == '__main__':
    if '--no-prepare' not in sys.argv: prepare()
    full_frames(); deltas(); windows()
