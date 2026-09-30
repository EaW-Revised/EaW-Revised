#pragma once

// Script fixture for the persistence tests (#248): every kind of state a save
// must carry. Cycles and aliases, metatables, closures sharing upvalues, an
// open upvalue on a suspended coroutine and one on an abandoned coroutine,
// coroutine.wrap and gfind and pairs cursors held across ticks, handle and
// function keys, table.insert sizes, thread slots that finish, fail, get
// killed or restart, skewed event queues, timers, registrations and a module
// whose initializer issues a command. An independent fixture, not FoC code.

namespace eawr::script::authoritative::test {

inline constexpr char persist_kit[] = R"LUA(
Test_Report("kit loaded")
local counter = 0
local Kit = {}
function Kit.bump(amount)
    counter = counter + (amount or 1)
    return counter
end
function Kit.peek()
    return counter
end
function Kit.mod(value, base)
    while value >= base do value = value - base end
    return value
end
return Kit
)LUA";

inline constexpr char persist_main[] = R"LUA(
local Kit = require("Kit")
Self = Test_Self()
Test_Report("main loaded", Self, Test_Random(1000))

Graph = {name = "root"}
Graph.self = Graph
Graph.children = {Graph, {parent = Graph}}
Shared = {value = 1}
AliasA = {ref = Shared}
AliasB = {ref = Shared, list = {Shared, Shared}}

Vector = {}
Vector.__index = Vector
function Vector.new(x, y) return setmetatable({x = x, y = y}, Vector) end
function Vector.len2(v) return v.x * v.x + v.y * v.y end
Default = setmetatable({}, {__index = function(t, k) return "default " .. k end})

Keyed = {}
Callbacks = {}
Seen = {}
Queue = {}
Sized = {}
table.setn(Sized, 5)

Counter = coroutine.wrap(function(start)
    local n = start
    while true do
        n = n + 1
        coroutine.yield(n)
    end
end)
Counter(10)

Worker = coroutine.create(function(a, b)
    local total = a + b
    Peek = function() return total end
    while true do
        local step = coroutine.yield(total)
        total = total + step
    end
end)
coroutine.resume(Worker, 1, 2)

do
    local lost = coroutine.create(function()
        local hidden = 41
        Escaped = function() hidden = hidden + 1; return hidden end
        coroutine.yield()
    end)
    coroutine.resume(lost)
end

Words = string.gfind("alpha beta gamma delta epsilon zeta eta theta", "%a+")
Big = {}
for index = 1, 12 do Big["k" .. index] = index * 3 end
BigNext = pairs(Big)

function Nested(depth)
    if depth == 0 then
        return coroutine.yield(true)
    end
    local result = Nested(depth - 1)
    return result
end

function Wait_Ticks(count)
    local start = GetCurrentTime.Frame()
    while GetCurrentTime.Frame() - start < count do
        Nested(2)
    end
end

function On_Signal(value)
    Test_Report("signal", Self, value, Kit.bump(1))
end

function Pump_Thread()
    local reads = 0
    while true do
        Wait_Ticks(1)
        local event = GetEvent()
        while event do
            reads = reads + 1
            ThreadValue.Set("reads", reads)
            -- Parameters are read for every other event, so the queues skew.
            if Kit.mod(reads, 2) == 0 then
                event(GetEvent.Params())
            else
                event("skipped")
            end
            event = GetEvent()
        end
    end
end

function Short_Thread(label)
    Wait_Ticks(3)
    Test_Report("short done", label)
end

function Doomed_Thread()
    while true do
        Wait_Ticks(1)
        Test_Report("doomed alive", GetCurrentTime.Frame())
    end
end

function Broken_Thread()
    Wait_Ticks(2)
    local missing = nil
    missing.field = 1
end

function Again_Thread()
    Test_Report("again", GetCurrentTime.Frame(), ThreadValue("seen") or 0)
    ThreadValue.Set("seen", GetCurrentTime.Frame())
    return true
end

function Guarded_Thread()
    -- The VM refuses a yield across pcall: no C continuation can exist.
    local ok, message = pcall(coroutine.yield, true)
    Test_Report("guarded", ok and "yielded" or message)
    Wait_Ticks(1000)
end

function On_Timer(param)
    Test_Report("timer", Self, param, GetCurrentTime.Frame())
end

function On_Timer_Once(param)
    Test_Report("timer once", Self, param)
    Cancel_Event("Timer_Fired", On_Timer_Once)
end

function On_Attacked(unit, amount)
    Seen[unit] = (Seen[unit] or 0) + amount
    Test_Report("attacked", Self, tostring(unit), amount)
end

function Main_Thread(label)
    local tick = 0
    while true do
        tick = tick + 1
        local frame = GetCurrentTime.Frame()
        local key = {tick = tick}
        Keyed[key] = frame
        Callbacks[function() return tick end] = frame
        Test_Report("identity", tostring(key), tostring(Graph))
        Seen[Test_Handle(3, Test_Random(6))] = frame
        table.insert(Queue, frame)
        if table.getn(Queue) > 4 then table.remove(Queue, 1) end
        local _, total = coroutine.resume(Worker, frame)
        Test_Report("values", label, total, Peek(), Counter(), Escaped(), table.getn(Queue), table.getn(Sized), Kit.peek())
        Test_Report("word", Words() or "none")
        local big_key, big_value = BigNext()
        Test_Report("big", big_key or "done", big_value or 0)
        Test_Report("vector", Vector.len2(Vector.new(frame, 2)), Default.missing, Shared.value)
        Shared.value = Shared.value + 1
        Test_Signal(Self, Signal_Slot, "On_Signal", frame)
        if Kit.mod(frame, 3) == 0 then Test_Timer(0.05 * frame, "Timer_Fired", frame) end
        if Kit.mod(frame, 5) == 1 then Register_Event("Timer_Fired", On_Timer_Once) end
        if frame == 6 then Create_Thread.Kill(Doomed) end
        local count = 0
        for k, v in pairs(Callbacks) do count = count + 1 end
        Test_Report("callbacks", count, Kit.mod(tick, 7))
        Wait_Ticks(1)
    end
end

Register_Event("Timer_Fired", On_Timer)
Register_Event("attacked", On_Attacked)
Create_Thread("Main_Thread", "main")
Signal_Slot = Create_Thread("Pump_Thread")
Create_Thread("Short_Thread", "short")
Doomed = Create_Thread("Doomed_Thread")
Create_Thread("Broken_Thread")
Create_Thread("Again_Thread")
Create_Thread("Guarded_Thread")
)LUA";

inline constexpr char persist_exit[] = R"LUA(
Test_Report("exit loaded", Test_Self())
function Exit_Thread()
    while GetCurrentTime.Frame() < 9 do
        coroutine.yield(true)
    end
    Test_Report("exiting")
    _ScriptExit()
    coroutine.yield(true)
end
Create_Thread("Exit_Thread")
)LUA";

} // namespace eawr::script::authoritative::test
