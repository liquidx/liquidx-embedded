#pragma once

class GfxRenderer;

// Font IDs are arbitrary non-zero keys into the renderer's font map.
namespace fonts {

constexpr int UI_10 = 1;    // Ubuntu 10: small labels, status
constexpr int UI_12 = 2;    // Ubuntu 12: (unused for now)
constexpr int BODY_14 = 3;  // Noto Sans 14: content text
constexpr int TITLE_18 = 4; // Noto Sans 18: content headings

void registerAll(GfxRenderer& renderer);

}  // namespace fonts
