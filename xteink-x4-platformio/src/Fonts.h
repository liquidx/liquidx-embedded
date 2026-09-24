#pragma once

class GfxRenderer;

// Font IDs are arbitrary non-zero keys into the renderer's font map. IBM Plex
// Mono in three sizes only (docs/design/README.md, scripts/fonts.sh); pixels.
namespace fonts {

constexpr int SMALL_15    = 1;  // 15px regular: rows, pills, captions, body copy
constexpr int LABEL_15    = 2;  // 15px regular tracked +0.08em: uppercase section pills
constexpr int MEDIUM_22   = 3;  // 22px medium: home rows, headings, unit suffixes
constexpr int DISPLAY_136 = 4;  // 136px regular tracked -0.05em: clock, setting value

void registerAll(GfxRenderer& renderer);

}  // namespace fonts
