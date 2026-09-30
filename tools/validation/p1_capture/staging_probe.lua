-- EaWR staging probe (#351 banking, #81 deaths). One function, EaWR_Stage_Service(), that the
-- capture mod's --staging-probe calls on every service of a space battle, and
-- EaWR_Stage_Reset(), which it calls at each space battle's start and end. Lua 5.0 in FoC's
-- GameScoring state; every game call goes through pcall and its result is checked, so a
-- missing function or a refused call shows up as an ERR advisor line (held through the
-- following wait) instead of stopping the script, and the case it belonged to is abandoned:
-- a line never names a step whose call did not go through.
--
-- A still is one instant of a cycle the probe repeats, so each still has to say which
-- moment it shows. The probe therefore lowers the script's service rate to 0.1 s (retail
-- reads ServiceRate on every service) and posts a short advisor line every post_every
-- services. Game_Message posts as urgent, which replaces the line on screen at once, so
-- the line in a still is at most post_every services old. Times are game seconds since
-- the step the line names.
--
-- FoC's script state has no math library (docs/lua-numeric-profile.md), so the square root and
-- the angle below are computed here.
--
-- Scenarios (EaWR_Stage_Scenario):
--   bank     A Corellian corvette, then a Nebulon-B, for the local player at the local
--            Rebel station: a first leg, a stop, then turns ordered 90 degrees to the
--            ship's left from rest, each held 14 s and followed by a stop.
--            Line: "BK <ship> <leg> +<s> a<deg>", a = heading change since the order,
--            measured from the ship's motion (positive: counter-clockwise from above).
--   deaths   A Tartan, then an Acclamator, for the enemy player at the
--            local station, one at a time: the script takes the hull down to about 10 %
--            in small steps and the station's own weapons make the kill, because the
--            death clone is chosen by the killing damage type (Damage_Normal here) and a
--            script kill does not carry it. Line: "DT <ship> <state> +<s>".
--   fighters An X-wing squadron, then a TIE squadron, the same way; every craft is taken
--            down to about 10 %. Line: "FT <craft> k<kills> +<s since the last kill>".
--   victory  #453: after start_wait the enemy station takes a killing hit, so the local player
--            wins: the win message, then 7 s later the battle end dialog, which stays up.
--   defeat   The same with the local Rebel station: the lose message and the dialog.
--            Line: "VD <victory|defeat> +<s since the kill>".
--   abilities #559: a Corellian corvette (TURBO, flying a long leg), a Nebulon-B (DEFEND) and an
--            X-wing squadron (SPOILER_LOCK) for the local player, one at a time: the ability is
--            switched on by script (Activate_Ability), held, then switched off, and the ship is
--            watched for a while after. Line: "AB <ship> <on|off|run> +<s since the phase began>
--            v<speed in units per frame over the last half second>".
--   turbo    #559: the corvette alone, on a shorter cycle. Its line also lists the speed sampled every
--            0.4 s since the switch ("AB CC off +5.4 v3.72 s7.4 6.9 6.2 ..."), so one still shows the
--            whole ramp of the speed after TURBO goes on or off.
--   reticle  #515: a Nebulon-B, then a Corellian corvette, for the local player, each moved
--            reticle_leg units off the local station, stopped and held reticle_hold seconds
--            with the camera on it, then the local station itself for as long. The route
--            parks the pointer at the screen centre, where the camera looks, so the held
--            subject is hovered and shows its hardpoint reticles.
--            Line: "RT <NB|CC|ST> <leg|hold> +<s>".
--   pause    #532: the AI is not suspended. The local fleet goes to the enemy and is destroyed
--            there; later local launches are removed by script. Line: "PZ t<now> f<fleet alive>
--            k<last fleet loss> n<first enemy near the local station> m<three near> h<first
--            station damage>", game seconds (EaWR_Stage_Pause).
--   pause_wings  The same without the corvette: the X-wings alone, so the wipe comes earlier.
--   aiturn   #668: an Acclamator for the enemy AI with a local Nebulon-B dead astern, staged with the AI
--            suspended; then the AI resumes and the Acclamator is watched (EaWR_Stage_Ai_Turn). Line:
--            "AT c<cycle> +<s> a<heading change> d<moved> tt<s> tm<s> th<s>"; the AI log gets the timeline.
--   findmask #532: once, after the start wait, Find_Nearest from the local station over the
--            enemy's objects with single names and '|' lists (FH-20); one second later an
--            unknown name. Line: "FM s<service> <key>=<type>@<distance>|nil|E ... X=<result>",
--            also written with DebugMessage; a frozen s<service> after X means the unknown
--            name ended the script.
--   melee    #601: the melee benchmark's size S (Rebel 11 ships and 10 squadrons, Empire 11 and
--            10) placed in two lines across the map centre and set on each other, again and
--            again (EaWR_Stage_Melee). Line: "ML c<cycle> <wait|gather|settle|fight> +<s>
--            R<ships>/<squadrons> E<ships>/<squadrons> <T|F|G teleport, M moved> t<game s>".
-- A kill the station does not make within kill_timeout seconds is forced by script and
-- the line says "SK" from then on, so its stills can be set aside.

EaWR_Stage = {
	scenario = EaWR_Stage_Scenario or "bank",
	station = "Skirmish_Rebel_Star_Base_1",
	enemy_station = "Skirmish_Empire_Star_Base_1",
	killed_at = nil,     -- victory and defeat: when the station took its killing hit
	service_rate = 0.1,
	pi = 3.14159265358979,
	post_every = 2,
	camera_every = 5,
	start_wait = 6,      -- seconds before the first spawn (the battle settles)
	-- bank
	bank_ships = {{"Corellian_Corvette", "CC"}, {"Nebulon_B_Frigate", "NB"}},
	bank_turns = 3,      -- turns per ship
	leg0_time = 9,       -- the first leg, towards the map centre
	rest_time = 5,       -- from a stop to the next turn order
	turn_time = 14,      -- from a turn order to the stop
	turn_distance = 2500,
	leg0_distance = 3000,
	-- abilities
	ability_cases = {
		{"Corellian_Corvette", "TURBO", "CC", true},
		{"Nebulon_B_Frigate", "DEFEND", "NB", false},
		{"Rebel_X-Wing_Squadron", "SPOILER_LOCK", "XW", false},
	},
	turbo_cases = {{"Corellian_Corvette", "TURBO", "CC", true}},
	turbo_lead = 3,
	turbo_hold = 5,
	turbo_after = 6,
	sample_every = 0.4,   -- seconds between the speeds listed in a turbo line
	sample_count = 9,
	samples = {},
	samples_taken = 0,
	ability_lead = 4,     -- seconds flying (or standing) before the ability goes on
	ability_hold = 9,     -- seconds the ability stays on
	ability_after = 9,    -- seconds watched after it goes off
	ability_distance = 8000,
	-- deaths and fighters
	-- The corvette's retail death is on owner footage (#81), so the stills go to these two.
	death_ships = {{"Tartan_Patrol_Cruiser", "TT", 500}, {"Acclamator_Assault_Ship", "AC", 2000}},
	fighter_squadrons = {{"Rebel_X-Wing_Squadron", "X-Wing", "XW", 60},
	                     {"TIE_Fighter_Squadron", "TIE_Fighter", "TF", 50}},
	hull_floor = 0.1,    -- the script stops damaging at this hull fraction
	damage_step = 0.04,  -- of the unit's hull health per service
	damage_time = 8,     -- seconds of stepping at most
	kill_timeout = 20,   -- seconds after the damage before a forced kill
	after_death = 5,     -- seconds to watch a ship's death
	after_last_kill = 5, -- seconds to watch after a squadron's last kill
	squadron_time = 25,  -- seconds from the first kill before the rest is removed
	-- reticle (the station entry has no type: the local station is held where it stands)
	reticle_subjects = {{"Nebulon_B_Frigate", "NB"}, {"Corellian_Corvette", "CC"}, {nil, "ST"}},
	reticle_leg = 2500,  -- units off the station, towards the map centre
	reticle_leg_time = 12,
	reticle_hold = 300,  -- seconds each subject is held under the camera
	-- pause (#532)
	pause_point = {2200, -2700}, -- where the fleet goes, about 2000 from the Empire station
	pause_near = 3000,   -- an enemy ship or craft this close to the local station counts as near
	-- melee (#601): apps/path_bench/melee.cpp's size S and layout; {type, count, squadron}
	melee_rebel = {{"Corellian_Corvette", 4}, {"Nebulon_B_Frigate", 4}, {"Calamari_Cruiser", 3},
	               {"Rebel_X-Wing_Squadron", 6, true}, {"Y-Wing_Squadron", 4, true}},
	melee_empire = {{"Tartan_Patrol_Cruiser", 6}, {"Acclamator_Assault_Ship", 5},
	                {"TIE_Fighter_Squadron", 4, true}, {"TIE_Interceptor_Squadron", 3, true},
	                {"TIE_Bomber_Squadron", 3, true}},
	melee_gap = 1400,
	melee_ship_spacing = 320,
	melee_rank_depth = 380,
	melee_squadron_spacing = 260,
	melee_squadron_depth = 240,
	melee_gather = 150,  -- seconds for moved units to reach their places
	melee_settle = 2,    -- seconds after a teleport
	melee_fight = 240,   -- seconds at most per fight
	-- aiturn (#668)
	aiturn_astern = 1000, -- the Nebulon-B stops this far behind the Acclamator (its attack distance is 1400)
	aiturn_watch = 60,    -- seconds the Acclamator is watched after the AI resumes
	aiturn_overshoot = 1500, -- its first leg ends this far past the midpoint, towards the local side
	-- state (EaWR_Stage_Reset sets it again for each battle)
	service = 0,
	started = nil,
	ai_suspended = false,
	next_index = 1,
	phase = "wait",
	phase_start = 0,
	wait_for = 6,        -- seconds this wait lasts (start_wait for the first)
	line = nil,
	err = nil,           -- the last ERR line, shown until the next spawn
	post_wait = 0,
	camera_wait = 0,
	history = {},
	victims = {},
}

-- Game seconds. The engine writes LastService, the game time of the script's previous
-- service, before it pumps the script, so this is that time plus one service. The rig
-- stills show GetCurrentTime advancing with it; it and the service count are fallbacks.
function EaWR_Stage_Now()
	if type(LastService) == "number" then
		return LastService + EaWR_Stage.service_rate
	end
	local ok, value = pcall(GetCurrentTime)
	if ok and type(value) == "number" and value > 0 then
		return value
	end
	return EaWR_Stage.service * EaWR_Stage.service_rate
end

-- The wait's line names both clocks and the service count, so a still shows which advance.
-- After a failed call it keeps the ERR line instead.
function EaWR_Stage_Wait_Line(prefix, age)
	if EaWR_Stage.err then
		EaWR_Stage.line = EaWR_Stage.err
		return
	end
	local ok, game = pcall(GetCurrentTime)
	local function short(value)
		return type(value) == "number" and string.format("%.1f", value) or tostring(value)
	end
	EaWR_Stage.line = prefix .. " wait +" .. string.format("%.1f", age) .. " L" ..
		short(LastService) .. " g" .. short(ok and game) .. " s" .. EaWR_Stage.service
end

function EaWR_Stage_Sqrt(value)
	if value <= 0 then
		return 0
	end
	local root = value > 1 and value / 2 or 1
	for i = 1, 40 do
		root = 0.5 * (root + value / root)
	end
	return root
end

-- atan on [-1, 1], within 1e-5 rad (a minimax polynomial).
function EaWR_Stage_Atan_Unit(z)
	local z2 = z * z
	return z * (0.9998660 + z2 * (-0.3302995 + z2 * (0.1801410 + z2 * (-0.0851330 + z2 * 0.0208351))))
end

function EaWR_Stage_Atan2(y, x)
	local pi = EaWR_Stage.pi
	local ax = x < 0 and -x or x
	local ay = y < 0 and -y or y
	if ax == 0 and ay == 0 then
		return 0
	end
	if ax >= ay then
		local angle = EaWR_Stage_Atan_Unit(y / x)
		if x < 0 then
			angle = angle + (y >= 0 and pi or -pi)
		end
		return angle
	end
	return (y >= 0 and pi / 2 or -pi / 2) - EaWR_Stage_Atan_Unit(x / y)
end

function EaWR_Stage_Valid(object)
	if not object then
		return false
	end
	local ok, valid = pcall(function() return object.Is_Valid() end)
	return ok and valid
end

function EaWR_Stage_Hull(object)
	local ok, hull = pcall(function() return object.Get_Hull() end)
	if ok and type(hull) == "number" then
		return hull
	end
	return nil
end

function EaWR_Stage_XY(object)
	local ok, x, y, z = pcall(function() return object.Get_Position().Get_XYZ() end)
	if ok and type(x) == "table" then
		x, y, z = x[1], x[2], x[3]
	end
	if ok and type(x) == "number" and type(y) == "number" then
		return x, y, (type(z) == "number") and z or 0
	end
	return nil
end

function EaWR_Stage_Seconds(since)
	local s = EaWR_Stage_Now() - since
	return string.format("%.1f", s)
end

function EaWR_Stage_Set_Phase(phase)
	EaWR_Stage.phase = phase
	EaWR_Stage.phase_start = EaWR_Stage_Now()
end

-- Posted at once, and held as the line until the next spawn.
function EaWR_Stage_Error(text)
	local d = EaWR_Stage
	d.err = "ERR " .. string.sub(tostring(text), 1, 60)
	d.line = d.err
	d.post_wait = d.post_every
	pcall(Game_Message, d.err)
end

-- A game call through pcall; an error or a false result is posted as "ERR <what> ..." and
-- the call reports false. Lua 5.0 varargs (arg), so the game function gets exactly the
-- arguments given.
function EaWR_Stage_Call(what, call, ...)
	local ok, result = pcall(call, unpack(arg))
	if not ok or result == false then
		EaWR_Stage_Error(what .. " " .. tostring(result))
		return false
	end
	return true
end

-- Removes the objects that are still valid; false (with an ERR line) when one could not be
-- despawned, in which case it is killed instead, as the debris probe does.
function EaWR_Stage_Dispose(objects)
	local disposed = true
	for i = 1, table.getn(objects) do
		local object = objects[i]
		local ok, valid = pcall(function() return object.Is_Valid() end)
		if not ok then
			EaWR_Stage_Error("valid " .. tostring(valid))
			disposed = false
		end
		if not ok or valid then
			if not EaWR_Stage_Call("despawn", function() return object.Despawn() end) then
				disposed = false
				EaWR_Stage_Call("cleanup-kill", function() return object.Take_Damage(1000000) end)
			end
		end
	end
	return disposed
end

-- The objects the current case owns.
function EaWR_Stage_Owned()
	local d = EaWR_Stage
	local owned = {}
	if d.ship then
		table.insert(owned, d.ship)
	end
	for i = 1, table.getn(d.victims) do
		table.insert(owned, d.victims[i])
	end
	return owned
end

-- Ends the current case after a failed call (which posted its ERR line; a reason posts
-- another): its objects go and the next spawn follows a short wait, the ERR line up meanwhile.
function EaWR_Stage_Abandon(reason)
	local d = EaWR_Stage
	if reason then
		EaWR_Stage_Error(reason)
	end
	EaWR_Stage_Dispose(EaWR_Stage_Owned())
	d.ship = nil
	d.victims = {}
	d.first_kill_time = nil
	EaWR_Stage_Set_Phase("wait")
	d.wait_for = 2
end

-- At each space battle's start and end: the objects of the case in progress go, and the
-- next battle starts from the first ship with the AI suspended again.
function EaWR_Stage_Reset()
	local d = EaWR_Stage
	EaWR_Stage_Dispose(EaWR_Stage_Owned())
	d.service = 0
	d.started = nil
	d.ai_suspended = false
	d.next_index = 1
	d.phase = "wait"
	d.phase_start = 0
	d.wait_for = d.start_wait
	d.line = nil
	d.err = nil
	d.post_wait = 0
	d.camera_wait = 0
	d.history = {}
	d.victims = {}
	d.ship = nil
	d.subject = nil
	d.tag = nil
	d.turn = nil
	d.heading = nil
	d.turn_heading = nil
	d.enemy = nil
	d.entry = nil
	d.health = nil
	d.kills = nil
	d.last_kill = nil
	d.killed_at = nil
	d.forced = nil
	d.alive_count = nil
	d.first_kill_time = nil
	d.fleet = {}
	d.fleet_alive = nil
	d.near_time = nil
	d.near3_time = nil
	d.hit_time = nil
	d.station_hull = nil
	d.station_shield = nil
	d.wiped = nil
	d.wipe_hull = nil
	d.wipe_shield = nil
	d.after_near3 = nil
	d.after_hit = nil
	EaWR_Stage_Dispose(EaWR_Stage_Melee_Objects())
	d.melee_sides = nil
	d.melee_form = nil
	d.cycle = nil
end

function EaWR_Stage_Show()
	local d = EaWR_Stage
	d.post_wait = d.post_wait - 1
	if d.post_wait > 0 or not d.line then
		return
	end
	d.post_wait = d.post_every
	pcall(Game_Message, d.line)
end

-- False when the camera call failed; the caller abandons the case, since its stills would
-- not show the ship the line names.
function EaWR_Stage_Camera(object, force)
	local d = EaWR_Stage
	d.camera_wait = d.camera_wait - 1
	if (d.camera_wait > 0 and not force) or not EaWR_Stage_Valid(object) then
		return true
	end
	d.camera_wait = d.camera_every
	return EaWR_Stage_Call("camera", Point_Camera_At, object)
end

function EaWR_Stage_Spawn(type_name, player)
	local ok, result = pcall(function()
		local anchor = Find_First_Object(EaWR_Stage.station)
		if not anchor then
			error("no " .. EaWR_Stage.station)
		end
		local units = Spawn_Unit(Find_Object_Type(type_name), anchor, player)
		if not units or not units[1] then
			error("spawn " .. type_name .. " returned nothing")
		end
		return units
	end)
	if not ok then
		EaWR_Stage_Error(result)
		return nil
	end
	return result
end

function EaWR_Stage_Players()
	local ok, localp = pcall(Find_Player, "local")
	if not ok or not localp then
		return nil, nil
	end
	local enemy = nil
	local okE, empire = pcall(Find_Player, "Empire")
	local okR, rebel = pcall(Find_Player, "Rebel")
	if okE and empire and empire ~= localp then
		enemy = empire
	elseif okR and rebel and rebel ~= localp then
		enemy = rebel
	end
	return localp, enemy
end

-- The heading of the ship's motion over the last half second, as a unit vector.
function EaWR_Stage_Track(ship)
	local d = EaWR_Stage
	local x, y = EaWR_Stage_XY(ship)
	if not x then
		return
	end
	table.insert(d.history, {EaWR_Stage_Now(), x, y})
	if table.getn(d.history) > 12 then
		table.remove(d.history, 1)
	end
end

function EaWR_Stage_Heading()
	local h = EaWR_Stage.history
	local n = table.getn(h)
	if n < 6 then
		return nil
	end
	local dx = h[n][2] - h[n - 5][2]
	local dy = h[n][3] - h[n - 5][3]
	local length = EaWR_Stage_Sqrt(dx * dx + dy * dy)
	if length < 3 then
		return nil
	end
	return dx / length, dy / length
end

function EaWR_Stage_Move(ship, x, y, z)
	return EaWR_Stage_Call("move", function() return ship.Move_To(Create_Position(x, y, z)) end)
end

function EaWR_Stage_Stop(ship)
	return EaWR_Stage_Call("stop", function() return ship.Stop() end)
end

function EaWR_Stage_Bank()
	local d = EaWR_Stage
	local now = EaWR_Stage_Now()
	local age = now - d.phase_start
	if d.phase == "wait" then
		EaWR_Stage_Wait_Line("BK", age)
		if age >= d.wait_for then
			EaWR_Stage_Set_Phase("spawn")
		end
		return
	end
	if d.phase == "spawn" then
		local entry = d.bank_ships[d.next_index]
		d.next_index = (d.next_index >= table.getn(d.bank_ships)) and 1 or d.next_index + 1
		local localp = EaWR_Stage_Players()
		local units = EaWR_Stage_Spawn(entry[1], localp)
		if not units then
			EaWR_Stage_Set_Phase("wait")
			return
		end
		d.ship = units[1]
		d.err = nil
		d.tag = entry[2]
		d.turn = 0
		d.history = {}
		if not EaWR_Stage_Camera(d.ship, true) then
			EaWR_Stage_Abandon()
			return
		end
		local x, y, z = EaWR_Stage_XY(d.ship)
		if not x then
			EaWR_Stage_Abandon("position " .. d.tag)
			return
		end
		-- Towards the map centre, or along +X when already near it.
		local length = EaWR_Stage_Sqrt(x * x + y * y)
		local ux, uy = 1, 0
		if length > 1500 then
			ux, uy = -x / length, -y / length
		end
		if not EaWR_Stage_Move(d.ship, x + ux * d.leg0_distance, y + uy * d.leg0_distance, z) then
			EaWR_Stage_Abandon()
			return
		end
		EaWR_Stage_Set_Phase("leg0")
		d.line = "BK " .. d.tag .. " leg0 +0.0"
		return
	end
	if not EaWR_Stage_Valid(d.ship) then
		d.line = "BK " .. tostring(d.tag) .. " lost"
		d.ship = nil
		EaWR_Stage_Set_Phase("wait")
		d.wait_for = 2
		return
	end
	EaWR_Stage_Track(d.ship)
	if not EaWR_Stage_Camera(d.ship) then
		EaWR_Stage_Abandon()
		return
	end
	local hx, hy = EaWR_Stage_Heading()
	if d.phase == "leg0" then
		if hx then
			d.heading = {hx, hy}
		end
		d.line = "BK " .. d.tag .. " leg0 +" .. string.format("%.1f", age)
		if age >= d.leg0_time then
			if not EaWR_Stage_Stop(d.ship) then
				EaWR_Stage_Abandon()
				return
			end
			EaWR_Stage_Set_Phase("rest")
		end
	elseif d.phase == "rest" then
		d.line = "BK " .. d.tag .. " rest" .. d.turn .. " +" .. string.format("%.1f", age)
		if age >= d.rest_time then
			if d.turn >= d.bank_turns then
				-- A failed despawn has posted its ERR line and killed the ship instead.
				EaWR_Stage_Dispose({d.ship})
				d.ship = nil
				EaWR_Stage_Set_Phase("wait")
				d.wait_for = 2
				return
			end
			local x, y, z = EaWR_Stage_XY(d.ship)
			local h = d.heading or {1, 0}
			if x then
				-- Left of the heading, counter-clockwise seen from above (+Z up).
				if not EaWR_Stage_Move(d.ship, x - h[2] * d.turn_distance, y + h[1] * d.turn_distance, z) then
					EaWR_Stage_Abandon()
					return
				end
				d.turn = d.turn + 1
				d.turn_heading = {h[1], h[2]}
				EaWR_Stage_Set_Phase("turn")
			end
		end
	elseif d.phase == "turn" then
		local angle = "?"
		if hx then
			d.heading = {hx, hy}
			local h0 = d.turn_heading
			local cross = h0[1] * hy - h0[2] * hx
			local dot = h0[1] * hx + h0[2] * hy
			angle = string.format("%+.0f", EaWR_Stage_Atan2(cross, dot) * 180 / EaWR_Stage.pi)
		end
		d.line = "BK " .. d.tag .. " t" .. d.turn .. " +" .. string.format("%.1f", age) .. " a" .. angle
		if age >= d.turn_time then
			if not EaWR_Stage_Stop(d.ship) then
				EaWR_Stage_Abandon()
				return
			end
			EaWR_Stage_Set_Phase("rest")
		end
	end
end

-- The ship's speed over the last half second, in units per frame (30 frames a second).
function EaWR_Stage_Speed()
	local h = EaWR_Stage.history
	local n = table.getn(h)
	if n < 6 then
		return 0
	end
	local dx = h[n][2] - h[n - 5][2]
	local dy = h[n][3] - h[n - 5][3]
	local dt = h[n][1] - h[n - 5][1]
	if dt <= 0 then
		return 0
	end
	return EaWR_Stage_Sqrt(dx * dx + dy * dy) / dt / 30
end

-- The speed over the last `count` services (0.1 s each), in units per frame.
function EaWR_Stage_Speed_Over(count)
	local h = EaWR_Stage.history
	local n = table.getn(h)
	if n <= count then
		return 0
	end
	local dx = h[n][2] - h[n - count][2]
	local dy = h[n][3] - h[n - count][3]
	local dt = h[n][1] - h[n - count][1]
	if dt <= 0 then
		return 0
	end
	return EaWR_Stage_Sqrt(dx * dx + dy * dy) / dt / 30
end

-- The turbo scenario's samples, one every sample_every seconds since the phase began, as text.
function EaWR_Stage_Samples(age)
	local d = EaWR_Stage
	while d.samples_taken < d.sample_count and (d.samples_taken + 1) * d.sample_every <= age do
		d.samples_taken = d.samples_taken + 1
		d.samples[d.samples_taken] = string.format("%.1f", EaWR_Stage_Speed_Over(2))
	end
	local text = ""
	for i = 1, table.getn(d.samples) do
		text = text .. (i > 1 and " " or "") .. d.samples[i]
	end
	return text
end

function EaWR_Stage_Ability(ship, ability, on)
	return EaWR_Stage_Call("ability " .. ability, function() return ship.Activate_Ability(ability, on) end)
end

function EaWR_Stage_Abilities()
	local d = EaWR_Stage
	local now = EaWR_Stage_Now()
	local age = now - d.phase_start
	if d.phase == "wait" then
		EaWR_Stage_Wait_Line("AB", age)
		if age >= d.wait_for then
			EaWR_Stage_Set_Phase("spawn")
		end
		return
	end
	if d.phase == "spawn" then
		local cases = (d.scenario == "turbo") and d.turbo_cases or d.ability_cases
		local entry = cases[d.next_index]
		d.next_index = (d.next_index >= table.getn(cases)) and 1 or d.next_index + 1
		local localp = EaWR_Stage_Players()
		local units = EaWR_Stage_Spawn(entry[1], localp)
		if not units then
			EaWR_Stage_Set_Phase("wait")
			return
		end
		d.ship = units[1]
		d.err = nil
		d.entry = entry
		d.tag = entry[3]
		d.history = {}
		if not EaWR_Stage_Camera(d.ship, true) then
			EaWR_Stage_Abandon()
			return
		end
		if entry[4] then
			local x, y, z = EaWR_Stage_XY(d.ship)
			if not x then
				EaWR_Stage_Abandon("position " .. d.tag)
				return
			end
			local length = EaWR_Stage_Sqrt(x * x + y * y)
			local ux, uy = 1, 0
			if length > 1500 then
				ux, uy = -x / length, -y / length
			end
			if not EaWR_Stage_Move(d.ship, x + ux * d.ability_distance, y + uy * d.ability_distance, z) then
				EaWR_Stage_Abandon()
				return
			end
		end
		EaWR_Stage_Set_Phase("run")
		d.line = "AB " .. d.tag .. " run +0.0 v0.00"
		return
	end
	if not EaWR_Stage_Valid(d.ship) then
		d.line = "AB " .. tostring(d.tag) .. " lost"
		d.ship = nil
		EaWR_Stage_Set_Phase("wait")
		d.wait_for = 2
		return
	end
	EaWR_Stage_Track(d.ship)
	if not EaWR_Stage_Camera(d.ship) then
		EaWR_Stage_Abandon()
		return
	end
	local turbo = d.scenario == "turbo"
	d.line = "AB " .. d.tag .. " " .. d.phase .. " +" .. string.format("%.1f", age) ..
		" v" .. string.format("%.2f", EaWR_Stage_Speed())
	if turbo and (d.phase == "on" or d.phase == "off") then
		d.line = d.line .. " s" .. EaWR_Stage_Samples(age)
	end
	local lead = turbo and d.turbo_lead or d.ability_lead
	local hold = turbo and d.turbo_hold or d.ability_hold
	local after = turbo and d.turbo_after or d.ability_after
	if d.phase == "run" then
		if age >= lead then
			if not EaWR_Stage_Ability(d.ship, d.entry[2], true) then
				EaWR_Stage_Abandon()
				return
			end
			d.samples = {}
			d.samples_taken = 0
			EaWR_Stage_Set_Phase("on")
		end
	elseif d.phase == "on" then
		if age >= hold then
			if not EaWR_Stage_Ability(d.ship, d.entry[2], false) then
				EaWR_Stage_Abandon()
				return
			end
			d.samples = {}
			d.samples_taken = 0
			EaWR_Stage_Set_Phase("off")
		end
	elseif d.phase == "off" then
		if age >= after then
			EaWR_Stage_Dispose({d.ship})
			d.ship = nil
			EaWR_Stage_Set_Phase("wait")
			d.wait_for = 2
		end
	end
end

-- Takes each living victim down towards hull_floor in steps; true once none is above it,
-- nil (with an ERR line) when a damage call failed.
function EaWR_Stage_Damage(victims, health)
	local d = EaWR_Stage
	local done = true
	for i = 1, table.getn(victims) do
		local victim = victims[i]
		if EaWR_Stage_Valid(victim) then
			local hull = EaWR_Stage_Hull(victim)
			if hull and hull > d.hull_floor then
				done = false
				local step = health * d.damage_step
				-- Never the killing blow: the station's weapons make it.
				if hull - step / health < d.hull_floor * 0.5 then
					step = (hull - d.hull_floor * 0.75) * health
				end
				if step > 0 and not EaWR_Stage_Call("damage", function() return victim.Take_Damage(step) end) then
					return nil
				end
			end
		end
	end
	return done
end

function EaWR_Stage_Living(victims)
	local living = {}
	for i = 1, table.getn(victims) do
		if EaWR_Stage_Valid(victims[i]) then
			local hull = EaWR_Stage_Hull(victims[i])
			if hull == nil or hull > 0 then
				table.insert(living, victims[i])
			end
		end
	end
	return living
end

function EaWR_Stage_Craft(craft_type, owner)
	local found = {}
	local ok, all = pcall(Find_All_Objects_Of_Type, craft_type)
	if not ok or not all then
		return found
	end
	for i = 1, table.getn(all) do
		local craft = all[i]
		local okOwner, craft_owner = pcall(function() return craft.Get_Owner() end)
		if EaWR_Stage_Valid(craft) and okOwner and craft_owner == owner then
			table.insert(found, craft)
		end
	end
	return found
end

-- #532 (FH-20): what Find_Nearest answers for one name, a '|' list of names and an unknown name.
EaWR_Stage_Mask_Filters = {
	{"S", "Structure"}, {"C", "Capital"}, {"SC", "Structure | Capital"}, {"CS", "Capital | Structure"},
	{"SCt", "Structure|Capital"}, {"F", "Fighter"}, {"FBC", "Fighter | Bomber | Corvette"},
	{"FrC", "Frigate | Capital"}, {"Fr", "Frigate"},
}

function EaWR_Stage_Mask_Probe(station, localp, filter)
	local ok, found = pcall(Find_Nearest, station, filter, localp, false)
	if not ok then
		return "E"
	end
	if not EaWR_Stage_Valid(found) then
		return "nil"
	end
	local okT, name = pcall(function() return found.Get_Type().Get_Name() end)
	local okD, distance = pcall(function() return station.Get_Distance(found) end)
	return (okT and tostring(name) or "?") .. "@" .. (okD and type(distance) == "number" and string.format("%d", distance) or "?")
end

function EaWR_Stage_Find_Mask()
	local d = EaWR_Stage
	local now = EaWR_Stage_Now()
	local localp = EaWR_Stage_Players()
	local ok, station = pcall(Find_First_Object, d.station)
	if not localp or not ok or not EaWR_Stage_Valid(station) then
		EaWR_Stage_Error("findmask: no player or station")
		return
	end
	if d.phase == "wait" then
		EaWR_Stage_Wait_Line("FM", now - d.phase_start)
		if now - d.phase_start < d.wait_for then
			return
		end
		local parts = {}
		for i = 1, table.getn(EaWR_Stage_Mask_Filters) do
			local entry = EaWR_Stage_Mask_Filters[i]
			table.insert(parts, entry[1] .. "=" .. EaWR_Stage_Mask_Probe(station, localp, entry[2]))
		end
		d.mask_result = table.concat(parts, " ")
		pcall(DebugMessage, "EAWR FM " .. d.mask_result)
		EaWR_Stage_Set_Phase("known")
	elseif d.phase == "known" and now - d.phase_start >= 1 then
		d.mask_unknown = EaWR_Stage_Mask_Probe(station, localp, "Eawr_Unknown_Name")
		pcall(DebugMessage, "EAWR FM X=" .. d.mask_unknown)
		EaWR_Stage_Set_Phase("done")
	end
	d.line = "FM s" .. d.service .. " " .. tostring(d.mask_result) .. " X=" .. tostring(d.mask_unknown)
end

-- #532: the enemy ships and craft (not structures) within pause_near of the local station.
function EaWR_Stage_Near(enemy, station)
	local sx, sy = EaWR_Stage_XY(station)
	local ok, all = pcall(Find_All_Objects_Of_Type, enemy)
	if not sx or not ok or not all then
		return 0
	end
	local near = 0
	local limit = EaWR_Stage.pause_near * EaWR_Stage.pause_near
	for i = 1, table.getn(all) do
		local object = all[i]
		local okS, structure = pcall(function() return object.Is_Category("Structure") end)
		if EaWR_Stage_Valid(object) and not (okS and structure) then
			local x, y = EaWR_Stage_XY(object)
			if x and (x - sx) * (x - sx) + (y - sy) * (y - sy) <= limit then
				near = near + 1
			end
		end
	end
	return near
end

-- #532: the enemy AI runs. After start_wait the local X-wing squadrons and a spawned Corellian
-- corvette (the fleet) attack-move at the enemy station (else move to pause_point); every later local X-wing or
-- Y-wing (the station's launches and replacements) is destroyed by script as it appears, so
-- once the enemy has destroyed the fleet the local mobile force stays zero. The line keeps the
-- game seconds of the fleet's last loss (k), of the first enemy ship or craft within
-- pause_near of the local station (n), of the first time three are (m) and of the station's
-- first damage (h); after the wipe, the enemy ships and craft near now (c), the first time three
-- are near again (a) and the station's first damage since (d).
-- Line: "PZ t<now> f<fleet alive> k<s> n<s> m<s> h<s> c<n> a<s> d<s>".
function EaWR_Stage_Pause()
	local d = EaWR_Stage
	local now = EaWR_Stage_Now()
	local localp, enemy = EaWR_Stage_Players()
	local ok, station = pcall(Find_First_Object, d.station)
	if not localp or not enemy or not ok or not EaWR_Stage_Valid(station) then
		EaWR_Stage_Error("pause: no players or station")
		return
	end
	if d.phase == "wait" then
		EaWR_Stage_Wait_Line("PZ", now - d.phase_start)
		if now - d.phase_start < d.wait_for then
			return
		end
		d.fleet = {}
		if d.scenario == "pause" then
			local corvette = EaWR_Stage_Spawn("Corellian_Corvette", localp)
			if corvette then
				table.insert(d.fleet, corvette[1])
			end
		end
		local ordered = EaWR_Stage_Craft("Rebel_X-Wing_Squadron", localp)
		for i = 1, table.getn(ordered) do
			table.insert(d.fleet, ordered[i])
		end
		local craft = EaWR_Stage_Craft("X-Wing", localp)
		for i = 1, table.getn(craft) do
			table.insert(d.fleet, craft[i])
		end
		local okE, enemy_station = pcall(Find_First_Object, d.enemy_station)
		for i = 1, table.getn(d.fleet) do
			local unit = d.fleet[i]
			local moved = false
			if okE and EaWR_Stage_Valid(enemy_station) then
				local okA, result = pcall(function() return unit.Attack_Move(enemy_station) end)
				moved = okA and result ~= false
			end
			if not moved then
				local target = d.pause_point
				pcall(function() return unit.Move_To(Create_Position(target[1], target[2], 0)) end)
			end
		end
		d.station_hull = EaWR_Stage_Hull(station)
		local okShield, shield = pcall(function() return station.Get_Shield() end)
		d.station_shield = okShield and type(shield) == "number" and shield or nil
		EaWR_Stage_Set_Phase("run")
		return
	end
	-- Later local launches leave at once; the fleet is what the enemy has to destroy.
	local launched = {"X-Wing", "Y-Wing"}
	for n = 1, table.getn(launched) do
		local craft = EaWR_Stage_Craft(launched[n], localp)
		for i = 1, table.getn(craft) do
			local unit = craft[i]
			local member = false
			for j = 1, table.getn(d.fleet) do
				member = member or d.fleet[j] == unit
			end
			if not member then
				pcall(function() return unit.Take_Damage(1000000) end)
			end
		end
	end
	local living = EaWR_Stage_Living(d.fleet)
	local alive = table.getn(living)
	if d.fleet_alive and alive < d.fleet_alive then
		d.last_kill = now
	end
	d.fleet_alive = alive
	if alive == 0 and not d.wiped then
		-- After the wipe: the station's health then, and the next arrival and damage.
		d.wiped = now
		d.wipe_hull = EaWR_Stage_Hull(station)
		local okW, shieldW = pcall(function() return station.Get_Shield() end)
		d.wipe_shield = okW and type(shieldW) == "number" and shieldW or nil
	end
	local near = EaWR_Stage_Near(enemy, station)
	if near >= 1 and not d.near_time then
		d.near_time = now
	end
	if near >= 3 and not d.near3_time then
		d.near3_time = now
	end
	local hull = EaWR_Stage_Hull(station)
	local okShield, shield = pcall(function() return station.Get_Shield() end)
	local damaged = (hull and d.station_hull and hull < d.station_hull - 0.001)
	if okShield and type(shield) == "number" and d.station_shield and shield < d.station_shield - 0.001 then
		damaged = true
	end
	if damaged and not d.hit_time then
		d.hit_time = now
	end
	if d.wiped then
		if near >= 3 and not d.after_near3 then
			d.after_near3 = now
		end
		local after = (hull and d.wipe_hull and hull < d.wipe_hull - 0.001)
		if okShield and type(shield) == "number" and d.wipe_shield and shield < d.wipe_shield - 0.001 then
			after = true
		end
		if after and not d.after_hit then
			d.after_hit = now
		end
	end
	local function at(value)
		return value and string.format("%.0f", value) or "-"
	end
	d.line = "PZ t" .. at(now) .. " f" .. alive .. " k" .. at(d.last_kill) .. " n" .. at(d.near_time) ..
		" m" .. at(d.near3_time) .. " h" .. at(d.hit_time) .. " c" .. near .. " a" .. at(d.after_near3) ..
		" d" .. at(d.after_hit)
end

-- #668: an AI-owned Acclamator with a Rebel Nebulon-B dead astern, inside its attack distance.
-- With the AI suspended, the enemy's Acclamator flies from the enemy station to the midpoint
-- between the stations, overshoots it and flies back, so it rests there facing the enemy station; then a local Nebulon-B flies in
-- from the local side and stops aiturn_astern units behind it. The AI is then resumed (t0) and
-- the Acclamator watched for aiturn_watch seconds: its heading (from its hardpoint bones), how
-- far it moved, its attack target and the Nebulon-B's shield and hull. The first time its heading
-- is more than 10 degrees off (tt), it has moved 20 units (tm) and the Nebulon-B is damaged (th)
-- are kept, in seconds after t0. Every half second a line goes to the AI log (DebugMessage,
-- the debug build's AILog.txt): "EAWR AT c<cycle> t<s> a<deg> d<moved> tg<target> ao<orders>
-- sh<shield> hu<hull>"; the advisor shows "AT c<cycle> +<s> a<deg> d<moved> tt<s> tm<s> th<s>".
function EaWR_Stage_Spawn_At(type_name, anchor, player)
	local ok, result = pcall(function()
		local units = Spawn_Unit(Find_Object_Type(type_name), anchor, player)
		if not units or not units[1] then
			error("spawn " .. type_name .. " returned nothing")
		end
		return units[1]
	end)
	if not ok then
		EaWR_Stage_Error(result)
		return nil
	end
	return result
end

-- The Acclamator's heading in radians: from the middle of its aft hardpoint bones to the middle
-- of its forward ones (the model's long axis), or nil when a bone cannot be read.
function EaWR_Stage_Bone_Heading(ship)
	local function bone(name)
		local ok, x, y = pcall(function() return ship.Get_Bone_Position(name).Get_XYZ() end)
		if ok and type(x) == "table" then
			x, y = x[1], x[2]
		end
		if ok and type(x) == "number" and type(y) == "number" then
			return x, y
		end
		return nil
	end
	local flx, fly = bone("HP_F-L_Bone")
	local frx, fry = bone("HP_F-R_Bone")
	local blx, bly = bone("HP_B-L_Bone")
	local brx, bry = bone("HP_B-R_Bone")
	if not flx or not frx or not blx or not brx then
		return nil
	end
	local dx = (flx + frx - blx - brx) / 2
	local dy = (fly + fry - bly - bry) / 2
	if dx * dx + dy * dy < 1 then
		return nil
	end
	return EaWR_Stage_Atan2(dy, dx)
end

function EaWR_Stage_Degrees(radians)
	local pi = EaWR_Stage.pi
	while radians >= pi do
		radians = radians - 2 * pi
	end
	while radians < -pi do
		radians = radians + 2 * pi
	end
	return radians * 180 / pi
end

-- #668: whether the local player's first object of each type answers Is_Good_Against(ship)
-- ("-" when there is none, "E" on an error), the ship's hull and shield, and the type of the
-- deadly enemy the AI's FindDeadlyEnemy names for the ship when this script state has it.
EaWR_Stage_Good_Against_Types = {
	{"NB", "Nebulon_B_Frigate"}, {"YC", "Y_Wing_Squadron_Container"}, {"YS", "Y-Wing_Squadron"}, {"YW", "Y-Wing"},
	{"XS", "Rebel_X-Wing_Squadron"}, {"XW", "X-Wing"}, {"CC", "Corellian_Corvette"},
}

function EaWR_Stage_Good_Against(ship, localp)
	local parts = {}
	for i = 1, table.getn(EaWR_Stage_Good_Against_Types) do
		local entry = EaWR_Stage_Good_Against_Types[i]
		local answer = "-"
		local ok, all = pcall(Find_All_Objects_Of_Type, entry[2])
		if ok and all then
			for j = 1, table.getn(all) do
				local object = all[j]
				local okOwner, owner = pcall(function() return object.Get_Owner() end)
				if answer == "-" and EaWR_Stage_Valid(object) and okOwner and owner == localp then
					local okG, good = pcall(function() return object.Is_Good_Against(ship) end)
					answer = okG and tostring(good) or "E"
				end
			end
		elseif not ok then
			answer = "E"
		end
		table.insert(parts, entry[1] .. "=" .. answer)
	end
	local okH, hull = pcall(function() return ship.Get_Hull() end)
	local okS, shield = pcall(function() return ship.Get_Shield() end)
	table.insert(parts, "hu" .. (okH and string.format("%.3f", hull) or "E"))
	table.insert(parts, "sh" .. (okS and string.format("%.3f", shield) or "E"))
	local deadly = "E"
	local okD, enemy = pcall(FindDeadlyEnemy, ship)
	if okD then
		deadly = "nil"
		if enemy then
			local okN, name = pcall(function() return enemy.Get_Type().Get_Name() end)
			deadly = okN and tostring(name) or "?"
		end
	end
	table.insert(parts, "DE=" .. deadly)
	return table.concat(parts, " ")
end

function EaWR_Stage_Arrived(ship, x, y)
	local sx, sy = EaWR_Stage_XY(ship)
	if not sx then
		return false
	end
	local dx, dy = sx - x, sy - y
	return dx * dx + dy * dy < 60 * 60
end

function EaWR_Stage_Ai_Turn()
	local d = EaWR_Stage
	local now = EaWR_Stage_Now()
	local age = now - d.phase_start
	local localp, enemy = EaWR_Stage_Players()
	local okL, station = pcall(Find_First_Object, d.station)
	local okE, enemy_station = pcall(Find_First_Object, d.enemy_station)
	if not localp or not enemy then
		EaWR_Stage_Error("aiturn: no players")
		return
	end
	if not okL or not EaWR_Stage_Valid(station) then
		EaWR_Stage_Error("aiturn: no local station")
		return
	end
	if not okE or not EaWR_Stage_Valid(enemy_station) then
		EaWR_Stage_Error("aiturn: no enemy station")
		return
	end
	local cycle = d.next_index
	if d.phase == "wait" then
		EaWR_Stage_Wait_Line("AT", age)
		if age < d.wait_for then
			return
		end
		local lx, ly = EaWR_Stage_XY(station)
		local ex, ey = EaWR_Stage_XY(enemy_station)
		if not lx or not ex then
			EaWR_Stage_Error("aiturn: station positions")
			return
		end
		local ux, uy = lx - ex, ly - ey
		local length = EaWR_Stage_Sqrt(ux * ux + uy * uy)
		ux, uy = ux / length, uy / length
		d.at_point = {(lx + ex) / 2, (ly + ey) / 2}
		d.at_astern = {d.at_point[1] + ux * d.aiturn_astern, d.at_point[2] + uy * d.aiturn_astern}
		-- The first leg overshoots the midpoint towards the local side; the second flies back to
		-- it, so the Acclamator ends at rest facing the enemy station, the local side astern.
		d.at_beyond = {d.at_point[1] + ux * d.aiturn_overshoot, d.at_point[2] + uy * d.aiturn_overshoot}
		d.ship = EaWR_Stage_Spawn_At("Acclamator_Assault_Ship", enemy_station, enemy)
		if not d.ship or not EaWR_Stage_Move(d.ship, d.at_beyond[1], d.at_beyond[2], 0) then
			EaWR_Stage_Abandon()
			return
		end
		EaWR_Stage_Set_Phase("leg")
	elseif d.phase == "leg" then
		d.line = "AT c" .. cycle .. " leg +" .. string.format("%.1f", age)
		EaWR_Stage_Camera(d.ship)
		if EaWR_Stage_Arrived(d.ship, d.at_beyond[1], d.at_beyond[2]) and age > 5 or age > 200 then
			if not EaWR_Stage_Move(d.ship, d.at_point[1], d.at_point[2], 0) then
				EaWR_Stage_Abandon()
				return
			end
			EaWR_Stage_Set_Phase("back")
		end
	elseif d.phase == "back" then
		local x, y = EaWR_Stage_XY(d.ship)
		d.line = "AT c" .. cycle .. " back +" .. string.format("%.1f", age)
		EaWR_Stage_Camera(d.ship)
		-- At rest on the midpoint: two services a second apart at the same spot.
		local settled = false
		if x and d.at_last and EaWR_Stage_Arrived(d.ship, d.at_point[1], d.at_point[2]) then
			local dx, dy = x - d.at_last[1], y - d.at_last[2]
			settled = dx * dx + dy * dy < 1
		end
		if d.service - (d.at_last_service or 0) >= 10 then
			d.at_last = x and {x, y} or nil
			d.at_last_service = d.service
		end
		if (settled and age > 5) or age > 120 then
			local victim = EaWR_Stage_Spawn_At("Nebulon_B_Frigate", station, localp)
			if not victim or not EaWR_Stage_Move(victim, d.at_astern[1], d.at_astern[2], 0) then
				if victim then
					table.insert(d.victims, victim)
				end
				EaWR_Stage_Abandon()
				return
			end
			table.insert(d.victims, victim)
			EaWR_Stage_Set_Phase("approach")
		end
	elseif d.phase == "approach" then
		local victim = d.victims[1]
		d.line = "AT c" .. cycle .. " approach +" .. string.format("%.1f", age)
		EaWR_Stage_Camera(d.ship)
		if not EaWR_Stage_Valid(d.ship) or not EaWR_Stage_Valid(victim) then
			EaWR_Stage_Abandon("aiturn: lost a ship")
			return
		end
		if EaWR_Stage_Arrived(victim, d.at_astern[1], d.at_astern[2]) and age > 5 or age > 150 then
			if not EaWR_Stage_Call("resume-ai", Suspend_AI, 0) then
				EaWR_Stage_Abandon()
				return
			end
			d.at_h0 = EaWR_Stage_Bone_Heading(d.ship)
			d.at_x0, d.at_y0 = EaWR_Stage_XY(d.ship)
			d.at_shield0 = nil
			local okS, shield = pcall(function() return victim.Get_Shield() end)
			d.at_shield0 = okS and type(shield) == "number" and shield or nil
			d.at_hull0 = EaWR_Stage_Hull(victim)
			d.at_tt, d.at_tm, d.at_th = nil, nil, nil
			d.at_log = 0
			local target0 = "nil"
			local okT0, t0 = pcall(function() return d.ship.Get_Attack_Target() end)
			if okT0 and t0 then
				local okN0, name0 = pcall(function() return t0.Get_Type().Get_Name() end)
				target0 = okN0 and tostring(name0) or "?"
			end
			local okO0, orders0 = pcall(function() return d.ship.Has_Active_Orders() end)
			pcall(DebugMessage, "EAWR AT c" .. cycle .. " t0 now " .. string.format("%.1f", now) ..
				" h0 " .. string.format("%.1f", d.at_h0 and EaWR_Stage_Degrees(d.at_h0) or 999) ..
				" tg" .. target0 .. " ao" .. tostring(okO0 and orders0) ..
				" x" .. string.format("%.0f", d.at_x0 or 0) .. " y" .. string.format("%.0f", d.at_y0 or 0))
			EaWR_Stage_Set_Phase("watch")
		end
	elseif d.phase == "watch" then
		local victim = d.victims[1]
		EaWR_Stage_Camera(d.ship)
		local h = EaWR_Stage_Valid(d.ship) and EaWR_Stage_Bone_Heading(d.ship) or nil
		local a = (h and d.at_h0) and EaWR_Stage_Degrees(h - d.at_h0) or nil
		local x, y = nil, nil
		if EaWR_Stage_Valid(d.ship) then
			x, y = EaWR_Stage_XY(d.ship)
		end
		local moved = (x and d.at_x0) and EaWR_Stage_Sqrt((x - d.at_x0) * (x - d.at_x0) + (y - d.at_y0) * (y - d.at_y0)) or nil
		local shield, hull = nil, nil
		if EaWR_Stage_Valid(victim) then
			local okS, value = pcall(function() return victim.Get_Shield() end)
			shield = okS and type(value) == "number" and value or nil
			hull = EaWR_Stage_Hull(victim)
		end
		if a and (a > 10 or a < -10) and not d.at_tt then
			d.at_tt = age
		end
		if moved and moved > 20 and not d.at_tm then
			d.at_tm = age
		end
		local damaged = (shield and d.at_shield0 and shield < d.at_shield0 - 0.0001) or
			(hull and d.at_hull0 and hull < d.at_hull0 - 0.0001) or (not EaWR_Stage_Valid(victim))
		if damaged and not d.at_th then
			d.at_th = age
		end
		local function at(value)
			return value and string.format("%.1f", value) or "-"
		end
		d.line = "AT c" .. cycle .. " +" .. at(age) .. " a" .. at(a) .. " d" .. at(moved) .. " tt" .. at(d.at_tt) ..
			" tm" .. at(d.at_tm) .. " th" .. at(d.at_th)
		d.at_log = d.at_log - 1
		if d.at_log <= 0 then
			d.at_log = 5
			local target = "nil"
			local okT, t = pcall(function() return d.ship.Get_Attack_Target() end)
			if okT and t then
				local okN, name = pcall(function() return t.Get_Type().Get_Name() end)
				target = okN and tostring(name) or "?"
			end
			local okO, orders = pcall(function() return d.ship.Has_Active_Orders() end)
			pcall(DebugMessage, "EAWR AT c" .. cycle .. " t" .. at(age) .. " a" .. at(a) .. " d" .. at(moved) ..
				" tg" .. target .. " ao" .. tostring(okO and orders) .. " sh" .. at(shield) .. " hu" .. at(hull) ..
				" x" .. at(x) .. " y" .. at(y) ..
				" tt" .. at(d.at_tt) .. " tm" .. at(d.at_tm) .. " th" .. at(d.at_th))
			d.at_ga = (d.at_ga or 0) - 1
			if d.at_ga <= 0 then
				d.at_ga = 4
				pcall(DebugMessage, "EAWR GA c" .. cycle .. " t" .. at(age) .. " " .. EaWR_Stage_Good_Against(d.ship, localp))
			end
		end
		if age >= d.aiturn_watch then
			pcall(DebugMessage, "EAWR AT c" .. cycle .. " end tt" .. at(d.at_tt) .. " tm" .. at(d.at_tm) .. " th" .. at(d.at_th))
			EaWR_Stage_Call("suspend-ai", Suspend_AI, 1)
			EaWR_Stage_Dispose(EaWR_Stage_Owned())
			d.ship = nil
			d.victims = {}
			d.at_last = nil
			d.at_last_service = nil
			d.next_index = d.next_index + 1
			EaWR_Stage_Set_Phase("wait")
			d.wait_for = 3
		end
	end
end

function EaWR_Stage_Deaths(fighters)
	local d = EaWR_Stage
	local now = EaWR_Stage_Now()
	local age = now - d.phase_start
	local list = fighters and d.fighter_squadrons or d.death_ships
	local prefix = fighters and "FT " or "DT "
	if d.phase == "wait" then
		EaWR_Stage_Wait_Line(string.sub(prefix, 1, 2), age)
		if age >= d.wait_for then
			EaWR_Stage_Set_Phase("spawn")
		end
		return
	end
	if d.phase == "spawn" then
		local entry = list[d.next_index]
		d.next_index = (d.next_index >= table.getn(list)) and 1 or d.next_index + 1
		local _, enemy = EaWR_Stage_Players()
		if not enemy then
			EaWR_Stage_Error("no enemy player")
			EaWR_Stage_Set_Phase("wait")
			return
		end
		local units = EaWR_Stage_Spawn(entry[1], enemy)
		if not units then
			EaWR_Stage_Set_Phase("wait")
			return
		end
		d.err = nil
		d.enemy = enemy
		d.entry = entry
		d.tag = fighters and entry[3] or entry[2]
		d.health = fighters and entry[4] or entry[3]
		d.victims = fighters and {} or {units[1]}
		d.kills = 0
		d.last_kill = nil
		d.forced = ""
		d.alive_count = table.getn(d.victims)
		EaWR_Stage_Set_Phase("damage")
		d.line = prefix .. d.tag .. " spawn"
		return
	end
	if fighters and table.getn(d.victims) == 0 then
		-- The squadron's craft exist from the service after the spawn.
		d.victims = EaWR_Stage_Craft(d.entry[2], d.enemy)
		d.alive_count = table.getn(d.victims)
		if d.alive_count == 0 then
			if age > 3 then
				EaWR_Stage_Error("no " .. d.entry[2] .. " craft")
				EaWR_Stage_Set_Phase("wait")
			end
			return
		end
	end
	local living = EaWR_Stage_Living(d.victims)
	local alive = table.getn(living)
	if alive < d.alive_count then
		d.kills = d.kills + (d.alive_count - alive)
		d.last_kill = now
		d.alive_count = alive
	end
	if living[1] and (fighters or d.kills == 0) and not EaWR_Stage_Camera(living[1]) then
		EaWR_Stage_Abandon()
		return
	end
	local state
	if d.last_kill then
		state = (fighters and ("k" .. d.kills) or "dead") .. " +" .. EaWR_Stage_Seconds(d.last_kill)
	else
		state = d.phase .. " +" .. string.format("%.1f", age)
		if living[1] then
			local hull = EaWR_Stage_Hull(living[1])
			if hull then
				state = state .. " h" .. string.format("%.0f", hull * 100)
			end
		end
	end
	d.line = prefix .. d.tag .. d.forced .. " " .. state
	if d.phase == "damage" then
		local done = EaWR_Stage_Damage(living, d.health)
		if done == nil then
			EaWR_Stage_Abandon()
			return
		end
		if done or age >= d.damage_time then
			EaWR_Stage_Set_Phase("kill")
		end
	elseif d.phase == "kill" then
		local finished
		if fighters then
			finished = alive == 0 and d.last_kill and now - d.last_kill >= d.after_last_kill
			finished = finished or (d.first_kill_time and now - d.first_kill_time >= d.squadron_time)
		else
			finished = d.last_kill and now - d.last_kill >= d.after_death
		end
		if d.last_kill and not d.first_kill_time then
			d.first_kill_time = d.last_kill
		end
		if not d.last_kill and age >= d.kill_timeout then
			-- The line says SK only once the forced kill went through; a refused one ends the case.
			for i = 1, alive do
				local victim = living[i]
				if not EaWR_Stage_Call("kill", function() return victim.Take_Damage(1000000) end) then
					EaWR_Stage_Abandon()
					return
				end
			end
			d.forced = " SK"
		end
		-- Never stuck on one victim: the next spawn follows in any case.
		finished = finished or age >= d.kill_timeout + d.squadron_time
		if finished then
			-- A failed despawn has posted its ERR line and killed the craft instead.
			EaWR_Stage_Dispose(living)
			d.first_kill_time = nil
			d.victims = {}
			EaWR_Stage_Set_Phase("wait")
			d.wait_for = 1
		end
	end
end

-- #453: one killing hit on a station decides the battle (space-victory VT-02); nothing else is
-- staged, so the stills show the message and then the battle end dialog.
function EaWR_Stage_Outcome(win)
	local d = EaWR_Stage
	local now = EaWR_Stage_Now()
	local age = now - d.phase_start
	local tag = "VD " .. (win and "victory" or "defeat")
	if d.phase == "wait" then
		EaWR_Stage_Wait_Line(tag, age)
		if age >= d.wait_for then
			EaWR_Stage_Set_Phase("kill")
		end
		return
	end
	if d.phase == "kill" then
		local name = win and d.enemy_station or d.station
		local ok, station = pcall(Find_First_Object, name)
		if not ok or not EaWR_Stage_Valid(station) then
			EaWR_Stage_Error("no " .. name)
		else
			EaWR_Stage_Camera(station, true)
			if EaWR_Stage_Call("kill", function() return station.Take_Damage(100000000) end) then
				d.killed_at = now
			end
		end
		EaWR_Stage_Set_Phase("done")
		return
	end
	if d.err then
		d.line = d.err
	else
		d.line = tag .. (d.killed_at and (" +" .. EaWR_Stage_Seconds(d.killed_at)) or " not killed")
	end
end

-- #515: one subject at a time under the camera, still, so the parked pointer hovers it.
function EaWR_Stage_Reticle()
	local d = EaWR_Stage
	local now = EaWR_Stage_Now()
	local age = now - d.phase_start
	if d.phase == "wait" then
		EaWR_Stage_Wait_Line("RT", age)
		if age >= d.wait_for then
			EaWR_Stage_Set_Phase("spawn")
		end
		return
	end
	if d.phase == "spawn" then
		local entry = d.reticle_subjects[d.next_index]
		d.next_index = (d.next_index >= table.getn(d.reticle_subjects)) and 1 or d.next_index + 1
		d.tag = entry[2]
		d.err = nil
		if not entry[1] then
			local ok, station = pcall(Find_First_Object, d.station)
			if not ok or not EaWR_Stage_Valid(station) then
				EaWR_Stage_Error("no " .. d.station)
				EaWR_Stage_Set_Phase("wait")
				d.wait_for = 2
				return
			end
			d.subject = station
			if not EaWR_Stage_Camera(station, true) then
				d.subject = nil
				EaWR_Stage_Abandon()
				return
			end
			EaWR_Stage_Set_Phase("hold")
			d.line = "RT " .. d.tag .. " hold +0.0"
			return
		end
		local localp = EaWR_Stage_Players()
		local units = EaWR_Stage_Spawn(entry[1], localp)
		if not units then
			EaWR_Stage_Set_Phase("wait")
			d.wait_for = 2
			return
		end
		d.ship = units[1]
		d.subject = d.ship
		if not EaWR_Stage_Camera(d.ship, true) then
			d.subject = nil
			EaWR_Stage_Abandon()
			return
		end
		local x, y, z = EaWR_Stage_XY(d.ship)
		if not x then
			d.subject = nil
			EaWR_Stage_Abandon("position " .. d.tag)
			return
		end
		-- Towards the map centre, or along +X when already near it.
		local length = EaWR_Stage_Sqrt(x * x + y * y)
		local ux, uy = 1, 0
		if length > 1500 then
			ux, uy = -x / length, -y / length
		end
		if not EaWR_Stage_Move(d.ship, x + ux * d.reticle_leg, y + uy * d.reticle_leg, z) then
			d.subject = nil
			EaWR_Stage_Abandon()
			return
		end
		EaWR_Stage_Set_Phase("leg")
		d.line = "RT " .. d.tag .. " leg +0.0"
		return
	end
	if not EaWR_Stage_Valid(d.subject) then
		d.line = "RT " .. tostring(d.tag) .. " lost"
		d.ship = nil
		d.subject = nil
		EaWR_Stage_Set_Phase("wait")
		d.wait_for = 2
		return
	end
	if not EaWR_Stage_Camera(d.subject) then
		d.subject = nil
		EaWR_Stage_Abandon()
		return
	end
	d.line = "RT " .. d.tag .. " " .. d.phase .. " +" .. string.format("%.1f", age)
	if d.phase == "leg" then
		if age >= d.reticle_leg_time then
			if not EaWR_Stage_Stop(d.ship) then
				d.subject = nil
				EaWR_Stage_Abandon()
				return
			end
			EaWR_Stage_Set_Phase("hold")
		end
	elseif d.phase == "hold" and age >= d.reticle_hold then
		-- The station stays; a spawned ship goes (a failed despawn kills it instead).
		if d.ship then
			EaWR_Stage_Dispose({d.ship})
		end
		d.ship = nil
		d.subject = nil
		EaWR_Stage_Set_Phase("wait")
		d.wait_for = 2
	end
end

-- #601 melee: the melee benchmark's size S (apps/path_bench/melee.cpp) staged in the retail game.
-- Each side's ships and squadrons spawn at its own station, go to their places in two lines
-- front_gap apart across the map centre (a teleport when the engine takes one, else a move and
-- a gather wait), then every ship attacks the enemy ship at its own place in the order and every
-- squadron attack-moves onto the enemy line's centre. A cycle ends when a side has nothing left
-- or after melee_fight seconds; what is left goes and the next cycle starts.
function EaWR_Stage_Melee_Places(entries, direction)
	local d = EaWR_Stage
	local places = {}
	local column, rank = 0, 0
	local first = true
	for e = 1, table.getn(entries) do
		local entry = entries[e]
		local squadron = entry[3]
		if squadron and first then
			column, rank = 0, 0
			first = false
		end
		for copy = 1, entry[2] do
			local x
			local y
			if squadron then
				x = direction * (d.melee_gap / 2 + rank * d.melee_squadron_depth)
				y = (2 * column - 7) * d.melee_squadron_spacing / 2
			else
				x = direction * (d.melee_gap / 2 + 250 + rank * d.melee_rank_depth)
				y = (2 * column - 7) * d.melee_ship_spacing / 2
			end
			table.insert(places, {entry[1], x, y, squadron})
			column = column + 1
			if column == 8 then
				column = 0
				rank = rank + 1
			end
		end
	end
	return places
end

-- Puts the unit at the place: a teleport if one of the engine's forms takes it, else a move.
-- Returns the form that worked ("T", "F", "G") or "M" for a move, nil when nothing did.
function EaWR_Stage_Melee_Place(unit, x, y)
	local position = Create_Position(x, y, 0)
	local forms = {
		{"T", function() return unit.Teleport(position) end},
		{"F", function() return unit.Teleport_And_Face(position) end},
		{"G", function() return Teleport(unit, position) end},
	}
	for i = 1, table.getn(forms) do
		local ok, result = pcall(forms[i][2])
		if ok and result ~= false then
			local px, py = EaWR_Stage_XY(unit)
			if px and (px - x) * (px - x) + (py - y) * (py - y) < 250000 then
				return forms[i][1]
			end
		end
	end
	local ok, result = pcall(function() return unit.Move_To(position) end)
	if ok and result ~= false then
		return "M"
	end
	return nil
end

function EaWR_Stage_Melee_Count(side)
	local ships, squadrons = 0, 0
	for i = 1, table.getn(side) do
		local unit = side[i]
		if EaWR_Stage_Valid(unit.object) then
			local hull = EaWR_Stage_Hull(unit.object)
			if hull == nil or hull > 0 then
				if unit.squadron then
					squadrons = squadrons + 1
				else
					ships = ships + 1
				end
			end
		end
	end
	return ships, squadrons
end

function EaWR_Stage_Melee_Line(now)
	local d = EaWR_Stage
	local rs, rq = EaWR_Stage_Melee_Count(d.melee_sides[1])
	local es, eq = EaWR_Stage_Melee_Count(d.melee_sides[2])
	d.line = "ML c" .. d.cycle .. " " .. d.phase .. " +" .. EaWR_Stage_Seconds(d.phase_start) .. " R" .. rs .. "/" ..
		rq .. " E" .. es .. "/" .. eq .. " " .. tostring(d.melee_form) .. " t" .. string.format("%.0f", now)
	return rs + rq, es + eq
end

function EaWR_Stage_Melee_Objects()
	local d = EaWR_Stage
	local objects = {}
	for s = 1, table.getn(d.melee_sides or {}) do
		local side = d.melee_sides[s]
		for i = 1, table.getn(side) do
			table.insert(objects, side[i].object)
		end
	end
	return objects
end

function EaWR_Stage_Melee_Camera()
	local d = EaWR_Stage
	d.camera_wait = d.camera_wait - 1
	if d.camera_wait > 0 then
		return
	end
	d.camera_wait = d.camera_every
	-- The first living ship of either side, nearest the centre by placement order.
	for s = 1, 2 do
		local side = d.melee_sides[s]
		for i = 1, table.getn(side) do
			if not side[i].squadron and EaWR_Stage_Valid(side[i].object) then
				pcall(Point_Camera_At, side[i].object)
				return
			end
		end
	end
end

function EaWR_Stage_Melee()
	local d = EaWR_Stage
	local now = EaWR_Stage_Now()
	if d.phase == "wait" then
		EaWR_Stage_Wait_Line("ML", now - d.phase_start)
		if now - d.phase_start < d.wait_for then
			return
		end
		local localp, enemy = EaWR_Stage_Players()
		if not localp or not enemy then
			EaWR_Stage_Error("melee: no players")
			return
		end
		d.err = nil
		d.cycle = (d.cycle or 0) + 1
		d.melee_sides = {{}, {}}
		d.melee_form = nil
		local owners = {localp, enemy}
		local rosters = {d.melee_rebel, d.melee_empire}
		local anchors = {d.station, d.enemy_station}
		for s = 1, 2 do
			local places = EaWR_Stage_Melee_Places(rosters[s], s == 1 and -1 or 1)
			local okA, anchor = pcall(Find_First_Object, anchors[s])
			for i = 1, table.getn(places) do
				local place = places[i]
				local ok, units = pcall(function()
					return Spawn_Unit(Find_Object_Type(place[1]), okA and anchor or Find_First_Object(d.station), owners[s])
				end)
				if ok and units and units[1] then
					local form = EaWR_Stage_Melee_Place(units[1], place[2], place[3])
					if form and (d.melee_form == nil or form ~= "M") then
						d.melee_form = form
					end
					table.insert(d.melee_sides[s], {object = units[1], squadron = place[4], x = place[2], y = place[3]})
				else
					EaWR_Stage_Error("spawn " .. place[1] .. " " .. tostring(units))
				end
			end
		end
		EaWR_Stage_Set_Phase(d.melee_form == "M" and "gather" or "settle")
		return
	end
	EaWR_Stage_Melee_Camera()
	local rebels, empire = EaWR_Stage_Melee_Line(now)
	local age = now - d.phase_start
	if d.phase == "gather" or d.phase == "settle" then
		if age < (d.phase == "gather" and d.melee_gather or d.melee_settle) then
			return
		end
		-- Ship i attacks the enemy ship at the same place in the order (scaled to the enemy's
		-- count); every squadron attack-moves onto the enemy ships' centre.
		for s = 1, 2 do
			local side = d.melee_sides[s]
			local enemies = {}
			local centre_x = 0
			for i = 1, table.getn(d.melee_sides[3 - s]) do
				local unit = d.melee_sides[3 - s][i]
				if not unit.squadron then
					table.insert(enemies, unit)
					centre_x = centre_x + unit.x
				end
			end
			local count = table.getn(enemies)
			if count > 0 then
				centre_x = centre_x / count
			end
			local ships = 0
			for i = 1, table.getn(side) do
				if not side[i].squadron then
					ships = ships + 1
				end
			end
			local ship = 0
			for i = 1, table.getn(side) do
				local unit = side[i]
				if unit.squadron or count == 0 then
					pcall(function() return unit.object.Attack_Move(Create_Position(centre_x, 0, 0)) end)
				else
					-- enemies[floor(ship * count / ships) + 1] without a math library.
					local index = 1
					while index < count and index * ships <= ship * count do
						index = index + 1
					end
					local target = enemies[index].object
					local ok, result = pcall(function() return unit.object.Attack_Target(target) end)
					if not ok or result == false then
						pcall(function() return unit.object.Attack_Move(target) end)
					end
					ship = ship + 1
				end
			end
		end
		EaWR_Stage_Set_Phase("fight")
		return
	end
	if d.phase == "fight" and (rebels == 0 or empire == 0 or age >= d.melee_fight) then
		EaWR_Stage_Dispose(EaWR_Stage_Melee_Objects())
		d.melee_sides = {{}, {}}
		EaWR_Stage_Set_Phase("wait")
		d.wait_for = 3
	end
end

function EaWR_Stage_Service()
	local d = EaWR_Stage
	d.service = d.service + 1
	-- Retail reads the scoring script's ServiceRate on every service.
	ServiceRate = d.service_rate
	if not d.started then
		d.started = true
		d.line = "STG " .. tostring(d.scenario) .. " start"
		d.phase_start = EaWR_Stage_Now()
	end
	if not d.ai_suspended and d.scenario ~= "pause" and d.scenario ~= "pause_wings" then
		-- The AI player keeps its fleet home, so no fight crosses the staging; nothing is
		-- staged until that call goes through.
		if not EaWR_Stage_Call("suspend-ai", Suspend_AI, 1) then
			pcall(EaWR_Stage_Show)
			return
		end
		d.ai_suspended = true
	end
	-- An error in the staging itself shows as its line and never ends the scoring script.
	local ok, message = pcall(EaWR_Stage_Step)
	if not ok then
		EaWR_Stage_Error("step " .. tostring(message))
	end
	pcall(EaWR_Stage_Show)
end

function EaWR_Stage_Step()
	local d = EaWR_Stage
	if d.scenario == "bank" then
		EaWR_Stage_Bank()
	elseif d.scenario == "deaths" then
		EaWR_Stage_Deaths(false)
	elseif d.scenario == "fighters" then
		EaWR_Stage_Deaths(true)
	elseif d.scenario == "abilities" or d.scenario == "turbo" then
		EaWR_Stage_Abilities()
	elseif d.scenario == "pause" or d.scenario == "pause_wings" then
		EaWR_Stage_Pause()
	elseif d.scenario == "victory" or d.scenario == "defeat" then
		EaWR_Stage_Outcome(d.scenario == "victory")
	elseif d.scenario == "reticle" then
		EaWR_Stage_Reticle()
	elseif d.scenario == "findmask" then
		EaWR_Stage_Find_Mask()
	elseif d.scenario == "melee" then
		EaWR_Stage_Melee()
	elseif d.scenario == "aiturn" then
		EaWR_Stage_Ai_Turn()
	else
		EaWR_Stage_Error("unknown scenario " .. tostring(d.scenario))
	end
end
