local smooth = 0
local peak   = 0
local PEAK_HOLD_TICKS = 8
local peak_hold = 0

local function bar_color(level)
  if level < 0.6 then
    return 0, 220, 220
  elseif level < 0.85 then
    return 255, 200, 0
  else
    return 255, 40, 0
  end
end

function init(ctx)
  screen.clear()
  screen.text(floor(screen.width() / 2) - 20, floor(screen.height() / 2) - 10, "MIC", 3, 80, 80, 80)
  screen.flip()
end

function on_tick(ctx, dt_ms)
  local level = mic.level()
  smooth = smooth * 0.7 + level * 0.3

  if smooth >= peak then
    peak = smooth
    peak_hold = PEAK_HOLD_TICKS
  else
    if peak_hold > 0 then
      peak_hold = peak_hold - 1
    else
      peak = peak * 0.92
    end
  end

  local W = screen.width()
  local H = screen.height()
  -- bar grows from right edge leftward
  local bar_w  = floor(smooth * W)
  local peak_x = floor((1 - peak) * W)
  local r, g, b = bar_color(smooth)

  screen.clear()
  if bar_w > 0 then
    screen.fill_rect(W - bar_w, 0, bar_w, H, r, g, b)
  end
  -- peak marker: 3px wide vertical stripe
  if peak > 0.01 then
    local px = max(0, min(W - 3, peak_x))
    screen.fill_rect(px, 0, 3, H, 255, 255, 255)
  end
  screen.flip()
end
