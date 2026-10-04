-- Engine-free observer contract: baseline exclusion, simultaneous additions,
-- timestamp intervals, dead/structure filtering and explicit failed-service state.
local now = 10
GetCurrentTime = function() return now end
local function object(name, live, structure, x, mobile)
	return {
		Is_Valid = function() return live end,
		Despawn = function() live = false end,
		Is_Category = function(category)
			if category == "Structure" then return structure end
			return category == "Fighter" and not structure and mobile ~= false
		end,
		Get_Type = function() return {Get_Name = function() return name end} end,
		Get_Hull = function() return 1 end,
		Get_Shield = function() return 0 end,
		Get_Position = function() return {Get_XYZ = function() return x, 2, 3 end} end
	}
end
local baseline = object("baseline", true, false, 1)
local first = object("craft-a", true, false, 2)
local second = object("craft-b", true, false, 3)
local dead = object("dead", false, false, 4)
local station = object("station", true, true, 5)
local capital_station = object("SKIRMISH_EMPIRE_STAR_BASE_2", true, false, 6)
local projectile = object("PROJ_SHIP_SMALL_LASER_CANNON_GREEN", true, false, 7, false)
local all = {baseline, dead, station, capital_station, projectile}
Find_All_Objects_Of_Type = function() return all end
ER = {enemy = {}, seen = {}, additions = {}, service = 0, previous = 10, origin = 10, dropped = 0, objects = {}, snapshots = {}}
ER.seen[tostring(baseline)] = true
EaWR_Reinforce_Sample()
assert(table.getn(ER.additions) == 0 and ER.first == nil)
all = {station, baseline, first, second, dead}
now = 10.0333333333333
EaWR_Reinforce_Sample()
assert(table.getn(ER.additions) == 2 and ER.first == now)
assert(ER.additions[1].name == "craft-a" and ER.additions[2].name == "craft-b")
assert(ER.additions[1].previous == 10 and ER.additions[2].now == now)
assert(ER.additions[1].x == 2 and ER.additions[1].service == 2)
now = 11
EaWR_Reinforce_Sample()
assert(table.getn(ER.additions) == 2 and ER.service == 3 and ER.previous == 11)
Find_All_Objects_Of_Type = function() error("inventory failed") end
EaWR_Reinforce_Service()
assert(type(ER.error) == "string" and string.find(ER.error, "inventory failed"))
ER.error = nil
Find_All_Objects_Of_Type = function() return all end
for i, offset in ipairs({1, 3, 5, 6}) do
	now = ER.first + offset
	EaWR_Reinforce_Service()
end
assert(ER.complete == true and table.getn(ER.snapshots) == 4)
local sealed_time, sealed_service = ER.now, ER.service
all = {baseline, first, second, object("late-craft", true, false, 20)}
now = now + 100
EaWR_Reinforce_Service()
assert(ER.now == sealed_time and ER.service == sealed_service and table.getn(ER.additions) == 2)
all = {first, second, station, capital_station, projectile}
EaWR_Cap_Clear_Fleet({})
assert(not first.Is_Valid() and not second.Is_Valid())
assert(station.Is_Valid() and capital_station.Is_Valid() and projectile.Is_Valid())
return "PASS"
