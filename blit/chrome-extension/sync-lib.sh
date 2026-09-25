#!/bin/sh
# Copy the blit JS library (../js) into lib/, which the extension loads.
# Chrome can't load files from outside the extension's folder, so run this
# after changing ../js and before loading or reloading the extension.
set -e
cd "$(dirname "$0")"
mkdir -p lib
cp ../js/blit.js ../js/protocol.js ../js/raster.js ../js/dom-capture.js ../js/sim-display.js lib/
echo "Copied blit library into $(pwd)/lib"
