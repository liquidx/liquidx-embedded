#!/bin/sh
# Regenerate the built-in IBM Plex Mono headers in lib/EpdFont/builtinFonts/.
# Three sizes only (docs/design/README.md): display 136, medium 22, small 15.
set -e
cd "$(dirname "$0")/.."
F=fonts/IBMPlexMono
conv() { python3 scripts/fontconvert.py "$@"; }

conv plexmono_15_regular 15 $F/IBMPlexMono-Regular.ttf                  # small: rows, captions, body copy
conv plexmono_15_label 15 $F/IBMPlexMono-Regular.ttf --tracking 0.08     # small, tracked: uppercase pills
conv plexmono_22_medium 22 $F/IBMPlexMono-Medium.ttf                    # medium: home rows, headings, units
conv plexmono_136_display 136 $F/IBMPlexMono-Regular.ttf --tracking -0.05  # display: clock, setting value
