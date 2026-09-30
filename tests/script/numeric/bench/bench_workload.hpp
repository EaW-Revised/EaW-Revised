#pragma once

// Synthetic per-tick script workload for the #246 load budget. FoC's AI
// scripts cannot run yet (#79), so this mirrors the static operation mix of the
// space tactical AI closure in plan/inventories/lua-numeric.json: many
// comparisons, a few adds/subtracts, rare multiply/divide, PGBase's
// Dirty_Floor/Simple_Mod and DebugMessage formatting. It is deliberately
// heavier in arithmetic than the closure (a scoring loop over 20 units per
// plan step) so the measured soft-float share is an upper estimate.

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace eawr::script::numeric::bench {

inline constexpr std::string_view workload_source = R"lua(
local function Dirty_Floor(val) return string.format("%d", val) end
local function Simple_Mod(a, b) return a - b * Dirty_Floor(a / b) end
local function DebugMessage(...) return string.format(unpack(arg)) end

Units = {}
for i = 1, 20 do
  Units[i] = { health = 100 + i * 7.5, shield = i * 2.25, x = i * 13.7, y = -i * 4.1, threat = i / 3 }
end

local function PlanStep(seed, now)
  local target, best = nil, -1
  for i = 1, table.getn(Units) do
    local u = Units[i]
    local dx = u.x - 100
    local dy = u.y + 50
    local score = u.threat * 0.8 + u.health / (u.shield + 1) - (dx * dx + dy * dy) * 0.001
    if score > best and u.health > 0 then
      best = score
      target = u
    end
  end
  local roll = Simple_Mod(seed + 1, 100)
  local allowed = roll < 30
  if now >= 30 and allowed then
    target.health = target.health - 2.5
    if target.health <= 0 then target.health = 150 end
  end
  return DebugMessage("plan -- seed:%d roll:%d best:%s now:%s", seed, roll, tostring(best), tostring(now))
end

function Tick(tick, steps)
  local last
  for s = 1, steps do
    last = PlanStep(tick * 7 + s, tick / 30)
  end
  return last
end
)lua";

struct TickSamples {
    std::vector<std::int64_t> nanoseconds;
    std::string_view last_message;
};

} // namespace eawr::script::numeric::bench
