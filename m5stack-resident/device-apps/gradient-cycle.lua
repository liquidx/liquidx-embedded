local cols = 64
local rows = 36

local schemes = {
  {{255,0,0},   {255,255,0}},   -- red → yellow
  {{0,0,255},   {128,0,255}},   -- blue → purple
  {{0,255,255}, {255,0,255}},   -- cyan → magenta
  {{0,180,0},   {0,255,180}},   -- green → teal
  {{255,80,0},  {255,0,120}},   -- orange → pink
  {{100,0,180}, {255,120,0}},   -- purple → orange (sunset)
  {{0,240,150}, {180,255,0}},   -- teal → lime
  {{0,50,255},  {0,255,200}},   -- blue → cyan
  {{15,20,80},  {255,180,50}},  -- dark blue → gold
  {{140,80,255},{255,140,140}}, -- lavender → rose
}

local scheme_idx = 1
local base = 0
local speeds = {0.02, 0.05, 0.1, 0.2, 0.4}
local speed_idx = 3
local speed_dir = 1

local function tri(t)
  t = fract(t) * 2
  if t > 1 then t = 2 - t end
  return t
end

function on_tick(ctx, dt_ms)
  base = (base + (dt_ms / 1000) * speeds[speed_idx]) % 1
  local c1 = schemes[scheme_idx][1]
  local c2 = schemes[scheme_idx][2]
  screen.clear()
  for row = 0, rows - 1 do
    local y = floor(row * 135 / rows)
    local h = floor((row + 1) * 135 / rows) - y
    for col = 0, cols - 1 do
      local x = floor(col * 240 / cols)
      local w = floor((col + 1) * 240 / cols) - x
      
      local diag = (col / cols + row / rows) * 0.5
      local t = tri(diag + base)
      
      local r = floor(c1[1] + (c2[1] - c1[1]) * t)
      local g = floor(c1[2] + (c2[2] - c1[2]) * t)
      local b = floor(c1[3] + (c2[3] - c1[3]) * t)
      screen.fill_rect(x, y, w, h, r, g, b)
    end
  end
  screen.flip()
end

function on_event(ctx, e)
  if e.name == "button" then
    if e.index == 0 then
      scheme_idx = (scheme_idx % #schemes) + 1
    elseif e.index == 1 then
      if speed_idx == #speeds then
        speed_dir = -1
      elseif speed_idx == 1 then
        speed_dir = 1
      end
      speed_idx = speed_idx + speed_dir
    end
  end
end
