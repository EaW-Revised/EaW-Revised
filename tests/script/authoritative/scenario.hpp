#pragma once

// Handwritten representative script load for the equality test and the budget
// benchmark (#247): per instance, a PGBase-style library (threads that sleep by
// polling GetCurrentTime, pumped event queues, function-keyed timer tables
// serviced with pairs, object-keyed tables) and an AI-style plan loop that
// scores units, sorts candidates, formats debug text and draws random numbers.
// It is an independent fixture, not FoC code.

#include "eawr/script/authoritative/scheduler.hpp"

#include <cstdint>
#include <string>

namespace eawr::script::authoritative::test {

inline constexpr char scenario_library[] = R"LUA(
YieldCount = 0
TimerTable = {}
DeathTable = {}

function Sleep(seconds)
    ThreadValue.Set("StartTime", GetCurrentTime())
    while GetCurrentTime() - ThreadValue("StartTime") < seconds do
        PumpEvents()
    end
end

function PumpEvents()
    Service_Timers()
    YieldCount = YieldCount + 1
    coroutine.yield(true)
    local event = GetEvent()
    while event do
        local text = string.format("%s pumping %s", tostring(Script_Name), tostring(event))
        local params = GetEvent.Params()
        if params then
            event(unpack(params))
        else
            event()
        end
        event = GetEvent()
    end
end

function Register_Timer(func, timeout, param)
    if TimerTable[func] == nil then
        TimerTable[func] = {}
    end
    table.insert(TimerTable[func], {timeout = timeout, start_time = GetCurrentTime(), param = param})
end

function Service_Timers()
    for func, entries in pairs(TimerTable) do
        local found = false
        for index, entry in pairs(entries) do
            found = true
            if entry.timeout + entry.start_time < GetCurrentTime() then
                entries[index] = nil
                func(entry.param)
                return Service_Timers()
            end
        end
        if not found then
            TimerTable[func] = nil
            return Service_Timers()
        end
    end
end
)LUA";

inline constexpr char scenario_ai[] = R"LUA(
require("Library")
Script_Name = "plan"
Units = {}
Scores = {}
Messages = 0

function Clamp(value, low, high)
    if value < low then return low elseif value > high then return high end
    return value
end

function On_Timer(param)
    Messages = Messages + 1
    Test_Report("timer", param, GetCurrentTime.Frame())
end

function On_Attacked(unit, amount)
    Units[unit] = (Units[unit] or 0) + amount
end

function Plan_Step()
    local candidates = {}
    for unit, health in pairs(Units) do
        local score = health * 0.75 + Test_Unit() * 12.5 - 3
        score = Clamp(score / 1.5, -1000, 1000)
        Scores[unit] = score
        table.insert(candidates, {unit = unit, score = score})
    end
    table.sort(candidates, function(left, right)
        if left.score == right.score then return tostring(left.unit) < tostring(right.unit) end
        return left.score > right.score
    end)
    local best = candidates[1]
    if best then
        local text = string.format("best %s score %.3f of %d", tostring(best.unit), best.score, table.getn(candidates))
        Test_Report("plan", text)
    end
end

function Main_Thread()
    Register_Timer(On_Timer, 0.5, "half")
    Register_Timer(On_Timer, 1.25, "late")
    while true do
        Plan_Step()
        Sleep(0.2 + Test_Random(4) / 10)
    end
end

function Watch_Thread()
    while true do
        Units[Test_Handle(7, Test_Random(24))] = Test_Random(100)
        Sleep(0.1)
    end
end

Register_Event("attacked", On_Attacked)
Create_Thread("Main_Thread")
Create_Thread("Watch_Thread")
)LUA";

} // namespace eawr::script::authoritative::test
