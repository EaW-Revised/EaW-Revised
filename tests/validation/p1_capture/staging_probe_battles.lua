-- Drives tools/validation/p1_capture/staging_probe.lua (loaded first, in the same chunk) with
-- fake engine calls, as the capture mod's hooks do: EaWR_Stage_Reset() at each space battle's
-- start and end, EaWR_Stage_Service() on each 0.1 s service. Lua 5.0 in the FoC profile
-- state (base, string, table): run by `lua_numeric_tests script`, it returns "PASS" or the
-- failed checks.

Fake = {}
Failures = {}

-- A fake call fails (errors) when Fake.fail[name] is set and is refused (returns false) when
-- Fake.refuse[name] is set.
function Fake_Call(name)
	if Fake.fail[name] then
		error("fake " .. name .. " failure")
	end
	if Fake.refuse[name] then
		return false
	end
	return nil
end

function Fake_Reset()
	Fake = {posted = {}, abilities = {}, objects = {}, suspend_calls = 0, suspend_args = nil, fail = {}, refuse = {}, time = 0}
end

function Fake_Object(type_name, owner)
	local o = {type_name = type_name, owner = owner, valid = true, hull = 1, health = 500, despawned = false}
	o.Is_Valid = function() return o.valid end
	o.Get_Hull = function() return o.hull end
	o.Get_Owner = function() return owner end
	o.Get_Position = function()
		return {Get_XYZ = function() return 100, 200, 0 end}
	end
	o.Take_Damage = function(amount)
		if Fake_Call(amount >= 1000000 and "kill" or "damage") == false then
			return false
		end
		o.hull = o.hull - amount / o.health
		if o.hull <= 0 then
			o.hull = 0
			o.valid = false
		end
	end
	o.Despawn = function()
		if Fake_Call("despawn") == false then
			return false
		end
		o.valid = false
		o.despawned = true
	end
	o.Move_To = function(position)
		return Fake_Call("move")
	end
	o.Stop = function()
		return Fake_Call("stop")
	end
	o.Activate_Ability = function(ability, on)
		table.insert(Fake.abilities, {o.type_name, ability, on})
		return Fake_Call("ability")
	end
	o.Attack_Move = function(target)
		o.attack_target = target
		return Fake_Call("attack")
	end
	o.Get_Shield = function() return o.shield end
	o.Is_Category = function(name) return name == "Structure" and o.structure == true end
	table.insert(Fake.objects, o)
	return o
end

LOCAL = {name = "local"}
EMPIRE = {name = "Empire"}

function Game_Message(text)
	table.insert(Fake.posted, text)
end
function GetCurrentTime()
	return Fake.time
end
function Point_Camera_At(object)
	return Fake_Call("camera")
end
function Suspend_AI(...)
	Fake.suspend_calls = Fake.suspend_calls + 1
	Fake.suspend_args = arg.n
	return Fake_Call("suspend")
end
function Find_Player(name)
	if name == "Empire" then
		return EMPIRE
	end
	return LOCAL
end
function Find_First_Object(name)
	if Fake.stations and Fake.stations[name] then
		return Fake.stations[name]
	end
	return {name = name}
end
function Find_Object_Type(name)
	return {name = name}
end
function Spawn_Unit(object_type, anchor, player)
	return {Fake_Object(object_type.name, player)}
end
function Create_Position(x, y, z)
	return {x, y, z}
end
function Find_All_Objects_Of_Type(name)
	return {}
end

function Serve(count)
	for i = 1, count do
		LastService = Fake.time
		Fake.time = Fake.time + 0.1
		EaWR_Stage_Service()
	end
end

function Serve_Until(done, limit)
	for i = 1, limit do
		if done() then
			return true
		end
		Serve(1)
	end
	return done()
end

function Check(condition, text)
	if not condition then
		table.insert(Failures, text)
	end
end

function Posted(text)
	for i = 1, table.getn(Fake.posted) do
		if string.find(Fake.posted[i], text, 1, true) then
			return true
		end
	end
	return false
end

-- A new battle of the given scenario, as the start hook runs it.
function Battle(scenario)
	Fake_Reset()
	EaWR_Stage.scenario = scenario
	EaWR_Stage_Reset()
end

function Test_Second_Battle_Starts_Clean()
	Battle("deaths")
	Serve_Until(function() return EaWR_Stage.phase == "damage" end, 200)
	local first = Fake.objects[1]
	Check(first and first.type_name == "Tartan_Patrol_Cruiser", "battle 1 stages the Tartan first")
	Serve(5)
	Check(first and first.hull < 1, "battle 1 damages the Tartan")
	-- The battle ends while the Tartan is being staged.
	EaWR_Stage_Reset()
	Check(first and first.despawned, "the battle's end despawns the ship in staging")
	local d = EaWR_Stage
	Check(d.phase == "wait" and d.next_index == 1 and d.started == nil and d.ai_suspended == false and
		table.getn(d.victims) == 0 and d.line == nil and d.err == nil and d.last_kill == nil,
		"the battle's end resets the staging state")
	-- The next space battle.
	EaWR_Stage_Reset()
	local calls = Fake.suspend_calls
	local spawned = table.getn(Fake.objects)
	Serve(1)
	Check(Fake.suspend_calls == calls + 1, "battle 2 suspends the AI again")
	Check(Fake.suspend_args == 1, "Suspend_AI gets exactly one argument")
	Serve(55)
	Check(table.getn(Fake.objects) == spawned, "battle 2 waits start_wait before its first spawn")
	Serve_Until(function() return table.getn(Fake.objects) > spawned end, 20)
	local second = Fake.objects[spawned + 1]
	Check(second and second.type_name == "Tartan_Patrol_Cruiser", "battle 2 starts from the first ship again")
	Check(EaWR_Stage.phase == "damage", "battle 2 stages its first ship from the spawn")
end

function Test_Refused_Suspend_Holds_The_Staging()
	Battle("deaths")
	Fake.refuse.suspend = true
	Serve(80)
	Check(Posted("ERR suspend-ai"), "a refused Suspend_AI posts ERR")
	Check(table.getn(Fake.objects) == 0, "nothing is staged while the AI is not suspended")
	Fake.refuse.suspend = nil
	Serve(20)
	Check(table.getn(Fake.objects) == 1, "the staging starts once Suspend_AI goes through")
end

function Test_Failed_Damage_Abandons_The_Case()
	Battle("deaths")
	Fake.fail.damage = true
	Serve_Until(function() return Posted("ERR damage") end, 200)
	local ship = Fake.objects[1]
	Check(Posted("ERR damage"), "a failed damage call posts ERR")
	Check(ship and ship.despawned, "a failed damage call abandons (despawns) the ship")
	Check(EaWR_Stage.phase == "wait", "a failed damage call ends the case")
	Serve(10)
	Check(string.find(EaWR_Stage.line, "ERR damage", 1, true) == 1, "the ERR line stays up through the wait")
end

function Test_Refused_Forced_Kill_Never_Shows_SK()
	Battle("deaths")
	Fake.fail.kill = true
	Serve_Until(function() return Posted("ERR kill") end, 400)
	Check(Posted("ERR kill"), "a failed forced kill posts ERR")
	Check(not Posted(" SK"), "a failed forced kill never shows SK")
	Check(Fake.objects[1] and Fake.objects[1].despawned, "a failed forced kill abandons the ship")
	Battle("deaths")
	Fake.refuse.kill = true
	Serve_Until(function() return Posted("ERR kill") end, 400)
	Check(Posted("ERR kill false"), "a refused forced kill posts ERR")
	Check(not Posted(" SK"), "a refused forced kill never shows SK")
end

function Test_Forced_Kill_Shows_SK()
	Battle("deaths")
	Serve_Until(function() return Posted("DT TT SK dead") end, 400)
	Check(Posted("DT TT SK dead"), "a forced kill that went through shows SK dead")
	Check(not Posted("ERR"), "a clean forced kill posts no ERR")
end

function Test_Failed_Camera_Abandons_The_Case()
	Battle("bank")
	Fake.fail.camera = true
	Serve_Until(function() return Posted("ERR camera") end, 100)
	Check(Posted("ERR camera"), "a failed camera call posts ERR")
	Check(Fake.objects[1] and Fake.objects[1].despawned, "a failed camera call abandons the ship")
	Serve(5)
	Check(not Posted("leg0"), "a ship the camera was not pointed at never shows leg0")
	Battle("deaths")
	Serve_Until(function() return EaWR_Stage.phase == "damage" end, 200)
	Fake.refuse.camera = true
	Serve(10)
	Check(Posted("ERR camera false"), "a refused camera call posts ERR")
	Check(Fake.objects[1] and Fake.objects[1].despawned, "a refused camera call abandons the ship")
end

function Test_Refused_Move_Never_Shows_The_Leg()
	Battle("bank")
	Fake.refuse.move = true
	Serve_Until(function() return Posted("ERR move") end, 100)
	Check(Posted("ERR move false"), "a refused move posts ERR")
	Serve(5)
	Check(not Posted("leg0"), "a refused move never shows leg0")
	Check(Fake.objects[1] and Fake.objects[1].despawned, "a refused move abandons the ship")
end

function Test_Refused_Despawn_Kills_Instead()
	Battle("deaths")
	Serve_Until(function() return EaWR_Stage.phase == "damage" end, 200)
	Fake.refuse.despawn = true
	EaWR_Stage_Reset()
	local ship = Fake.objects[1]
	Check(Posted("ERR despawn false"), "a refused despawn posts ERR")
	Check(ship and not ship.despawned and ship.hull == 0, "a refused despawn kills the ship instead")
end

-- #453: victory and defeat give one station a killing hit, then only date the stills.
function Test_Outcome(scenario, name, refuse)
	Battle(scenario)
	local station = Fake_Object(name, LOCAL)
	table.remove(Fake.objects)
	Fake.stations = {}
	Fake.stations[name] = station
	Fake.refuse.kill = refuse
	Serve_Until(function() return EaWR_Stage.phase == "done" end, 200)
	Serve(30)
	if refuse then
		Check(station.valid, scenario .. ": a refused kill leaves the station")
		Check(Posted("ERR kill false") and string.find(EaWR_Stage.line, "ERR", 1, true) == 1,
			scenario .. ": a refused kill posts ERR and keeps it as the line")
		return
	end
	Check(not station.valid, scenario .. ": the station takes a killing hit")
	Check(Posted("VD " .. scenario .. " +"), scenario .. ": the line dates the stills from the kill")
	Check(not Posted("ERR"), scenario .. ": no ERR")
	Check(table.getn(Fake.objects) == 0, scenario .. ": nothing is spawned")
end

-- #559: each ship's ability goes on once, then off once, and the ship goes after the watch.
function Test_Abilities()
	Battle("abilities")
	Serve_Until(function() return table.getn(Fake.abilities) >= 6 end, 3000)
	local a = Fake.abilities
	Check(table.getn(a) >= 6, "abilities: three ships each switch on and off")
	Check(a[1][1] == "Corellian_Corvette" and a[1][2] == "TURBO" and a[1][3] == true, "abilities: TURBO on first")
	Check(a[2][1] == "Corellian_Corvette" and a[2][2] == "TURBO" and a[2][3] == false, "abilities: then TURBO off")
	Check(a[3][1] == "Nebulon_B_Frigate" and a[3][2] == "DEFEND" and a[3][3] == true, "abilities: DEFEND on")
	Check(a[5][1] == "Rebel_X-Wing_Squadron" and a[5][2] == "SPOILER_LOCK" and a[5][3] == true, "abilities: SPOILER_LOCK on")
	Check(Posted("AB CC on +"), "abilities: the line dates the on phase")
	Check(Posted("AB CC off +"), "abilities: the line dates the off phase")
	Check(not Posted("ERR"), "abilities: no ERR")
end

function Test_Refused_Ability_Abandons_The_Case()
	Battle("abilities")
	Fake.refuse.ability = true
	Serve(400)
	Check(Posted("ERR ability TURBO false"), "abilities: a refused switch posts ERR")
	Check(not Posted("AB CC on +"), "abilities: a refused switch never shows the on phase")
end

-- #559: the turbo scenario keeps the corvette only and lists the speed samples in its lines.
function Test_Turbo_Samples()
	Battle("turbo")
	Serve_Until(function() return table.getn(Fake.abilities) >= 2 end, 3000)
	Serve(60)
	Check(Fake.abilities[1][1] == "Corellian_Corvette" and Fake.abilities[1][3] == true, "turbo: TURBO on")
	Check(Fake.abilities[2][3] == false, "turbo: TURBO off")
	Check(Posted("AB CC off +"), "turbo: the off phase is dated")
	local listed = false
	for i = 1, table.getn(Fake.posted) do
		if string.find(Fake.posted[i], "AB CC off +", 1, true) and string.find(Fake.posted[i], " s0.0 0.0", 1, true) then
			listed = true
		end
	end
	Check(listed, "turbo: the off line lists the speed samples")
	for i = 1, table.getn(Fake.abilities) do
		Check(Fake.abilities[i][1] == "Corellian_Corvette", "turbo: only the corvette is staged")
	end
end

-- #532: pause leaves the AI running, sends the fleet at the enemy station, removes later local
-- launches and dates the fleet's last loss and the enemy's arrival and damage after it.
function Test_Pause()
	Battle("pause")
	local rebel_station = Fake_Object("Skirmish_Rebel_Star_Base_1", LOCAL)
	rebel_station.structure = true
	rebel_station.shield = 1
	local empire_station = Fake_Object("Skirmish_Empire_Star_Base_1", EMPIRE)
	empire_station.structure = true
	Fake.stations = {Skirmish_Rebel_Star_Base_1 = rebel_station, Skirmish_Empire_Star_Base_1 = empire_station}
	local wing = {Fake_Object("X-Wing", LOCAL), Fake_Object("X-Wing", LOCAL)}
	local all_of = Find_All_Objects_Of_Type
	Find_All_Objects_Of_Type = function(key)
		local found = {}
		for i = 1, table.getn(Fake.objects) do
			local o = Fake.objects[i]
			if o.valid and (o.type_name == key or o.owner == key) then
				table.insert(found, o)
			end
		end
		return found
	end
	Serve(70)
	Check(Fake.suspend_calls == 0, "pause: the AI keeps running")
	local fleet = EaWR_Stage.fleet
	Check(table.getn(fleet) == 3, "pause: the fleet is the spawned corvette and the X-wings")
	Check(wing[1].attack_target == empire_station, "pause: the fleet attack-moves at the enemy station")
	local launch = Fake_Object("Y-Wing", LOCAL)
	Serve(1)
	Check(not launch.valid, "pause: a later local launch is removed")
	Check(wing[1].valid, "pause: the fleet is left to the enemy")
	for i = 1, table.getn(fleet) do
		fleet[i].valid = false
	end
	Serve(10)
	Check(string.find(EaWR_Stage.line, " f0 k7 ", 1, true) ~= nil, "pause: the line dates the fleet's last loss: " .. EaWR_Stage.line)
	for i = 1, 3 do
		Fake_Object("Tartan_Patrol_Cruiser", EMPIRE)
	end
	rebel_station.shield = 0.5
	Serve(2)
	Check(string.find(EaWR_Stage.line, " c3 a8 d8", 1, true) ~= nil, "pause: the line dates the arrival and damage after the wipe: " .. EaWR_Stage.line)
	Check(not Posted("ERR"), "pause: no ERR")
	Find_All_Objects_Of_Type = all_of
	Fake.stations = nil
end

-- #532 (FH-20): findmask asks Find_Nearest once for each filter after the start wait, then an
-- unknown name a second later, and keeps the answers on its line and in the AI log.
function Test_Find_Mask()
	Battle("findmask")
	local station = Fake_Object("Skirmish_Rebel_Star_Base_1", LOCAL)
	Fake.stations = {Skirmish_Rebel_Star_Base_1 = station}
	local asked = {}
	local logged = {}
	Find_Nearest = function(source, filter, player, ally)
		table.insert(asked, filter)
		if filter == "Eawr_Unknown_Name" then
			error("unknown")
		end
		if filter == "Capital" then
			local ship = Fake_Object("Star_Destroyer", EMPIRE)
			ship.Get_Type = function() return {Get_Name = function() return "STAR_DESTROYER" end} end
			return ship
		end
		return nil
	end
	station.Get_Distance = function(other) return 1234.5 end
	DebugMessage = function(text) table.insert(logged, text) end
	Serve(90)
	Check(Fake.suspend_calls == 1, "findmask: the AI is suspended")
	Check(table.getn(asked) == table.getn(EaWR_Stage_Mask_Filters) + 1, "findmask: every filter and the unknown name are asked once")
	Check(string.find(EaWR_Stage.line, "C=STAR_DESTROYER@1234 SC=nil", 1, true) ~= nil, "findmask: the line names each answer: " .. EaWR_Stage.line)
	Check(string.find(EaWR_Stage.line, " X=E", 1, true) ~= nil, "findmask: an error is E: " .. EaWR_Stage.line)
	Check(table.getn(logged) == 2 and string.find(logged[1], "EAWR FM S=nil", 1, true) == 1, "findmask: the answers go to the AI log")
	Check(not Posted("ERR"), "findmask: no ERR")
	Find_Nearest = nil
	DebugMessage = nil
	Fake.stations = nil
end


-- #601: melee spawns size S for both sides, moves them to their places when no teleport takes,
-- sets every ship on its mirrored enemy ship and every squadron on the enemy line after the
-- gather wait, and starts the next cycle once a side is gone.
function Test_Melee()
	Battle("melee")
	Serve_Until(function() return EaWR_Stage.phase == "gather" end, 200)
	Serve(1)
	Check(table.getn(Fake.objects) == 42, "melee: 21 units a side spawn, got " .. table.getn(Fake.objects))
	Check(string.find(EaWR_Stage.line or "", "ML c1 gather", 1, true) ~= nil, "melee: the gather line: " .. tostring(EaWR_Stage.line))
	Check(string.find(EaWR_Stage.line or "", "R21/", 1, true) == nil and string.find(EaWR_Stage.line or "", "R11/10 E11/10 M", 1, true) ~= nil,
		"melee: the line counts ships and squadrons and says they moved: " .. tostring(EaWR_Stage.line))
	Serve_Until(function() return EaWR_Stage.phase == "fight" end, 2000)
	Check(EaWR_Stage.phase == "fight", "melee: the fight starts after the gather wait")
	local ordered = 0
	for i = 1, table.getn(Fake.objects) do
		if Fake.objects[i].attack_target then
			ordered = ordered + 1
		end
	end
	Check(ordered == 42, "melee: every unit has an attack order, got " .. ordered)
	local first = EaWR_Stage.melee_sides[1][1]
	Check(first.object.attack_target == EaWR_Stage.melee_sides[2][1].object, "melee: the first ship attacks the first enemy ship")
	for i = 1, table.getn(EaWR_Stage.melee_sides[2]) do
		EaWR_Stage.melee_sides[2][i].object.valid = false
	end
	Serve(3)
	Check(EaWR_Stage.phase == "wait", "melee: a side gone ends the cycle")
	Check(first.object.despawned, "melee: the survivors go")
	Serve_Until(function() return EaWR_Stage.cycle == 2 end, 100)
	Check(table.getn(Fake.objects) == 84, "melee: the second cycle spawns again")
	Check(not Posted("ERR"), "melee: no ERR")
end

Test_Abilities()
Test_Turbo_Samples()
Test_Refused_Ability_Abandons_The_Case()
Test_Pause()
Test_Find_Mask()
Test_Melee()
Test_Outcome("victory", "Skirmish_Empire_Star_Base_1", nil)
Test_Outcome("defeat", "Skirmish_Rebel_Star_Base_1", nil)
Test_Outcome("victory", "Skirmish_Empire_Star_Base_1", true)
Test_Second_Battle_Starts_Clean()
Test_Refused_Suspend_Holds_The_Staging()
Test_Failed_Damage_Abandons_The_Case()
Test_Refused_Forced_Kill_Never_Shows_SK()
Test_Forced_Kill_Shows_SK()
Test_Failed_Camera_Abandons_The_Case()
Test_Refused_Move_Never_Shows_The_Leg()
Test_Refused_Despawn_Kills_Instead()

if table.getn(Failures) > 0 then
	return "FAIL: " .. table.concat(Failures, "; ")
end
return "PASS"
