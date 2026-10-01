#!/bin/sh
# Regenerate the IBM Plex Mono headers in src/fonts/ (needs Pillow). The TTFs
# are the X4 project's.
set -e
cd "$(dirname "$0")/.."
F=../xteink-x4-platformio/fonts/IBMPlexMono
conv() { name=$1; shift; python3 scripts/vlwfont.py "$name" "$@" > "src/fonts/$name.h"; }

conv plexmono_12_regular 12 $F/IBMPlexMono-Regular.ttf                   # small: dates, captions, About
conv plexmono_11_label 11 $F/IBMPlexMono-Medium.ttf --tracking 0.06      # small, tracked: uppercase pills
conv plexmono_16_medium 16 $F/IBMPlexMono-Medium.ttf                     # medium: rows, headings
conv plexmono_30_large 30 $F/IBMPlexMono-Regular.ttf --tracking -0.03    # large: a setting's value
conv plexmono_46_display 46 $F/IBMPlexMono-Regular.ttf --tracking -0.05 --chars "0123456789:-"  # clock, pairing code
