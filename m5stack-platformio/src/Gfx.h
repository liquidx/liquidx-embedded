#pragma once

#include <M5GFX.h>

// Everything draws into one off-screen sprite the size of the panel, 16 bits a
// pixel, which is then pushed to the LCD whole: no flicker, and the same code
// renders on a computer (sim/).
using Gfx = M5Canvas;
