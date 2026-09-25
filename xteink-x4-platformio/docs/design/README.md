# XTeink UI — direction 3b

Device: 800×480 landscape, 1-bit e-ink. PNGs are 1:1 device pixels.

## Layout
- Canvas: x 0–728. Button gutter: x 728–800 (72px), plain white.
- Gutter: battery glyph at (751,24) 26×14; four 48px black circles at x=740, y=73/161/269/357: ◀ Back/Home, ● Select, ▲ Up, ▼ Down. Always all four shown, even when inert.
- Every page is a card: white, 1px black border, radius 22px on the right corners only, full height (bleeds off top/bottom/left).
- Back stack: depth n card width = 728 − 12n. Previous cards remain drawn beneath, so their right edges peek out beside the gutter (one edge per level).
- All selectable options are stacked vertically; selected item = black pill, white text. No horizontal choices.

## Type — three sizes only
IBM Plex Mono throughout.
- Display 136px, weight 400, letter-spacing −0.05em — clock, setting value.
- Medium 22px, weight 500 — anything with a highlight/selected state (home items, settings rows, option rows), setting titles, unit suffix ("pages").
- Small 15px, weight 400 — non-selectable text only: date pill, section pills (uppercase, tracking .08em), filenames, counters, descriptions.
Rows: home items 60px / radius 16; list rows 56px / radius 14; selected row = black fill, white text.

## Screens
1. home — date pill, clock, vertical list: Images (selected), Settings.
2. images — depth 1, photo fills card, filename + counter pills bottom-left.
3. settings — depth 1, full-width list with current value right-aligned.
4. settings-refresh — depth 2, value display left, vertical options right (6 / 12 / 24 / Never).
