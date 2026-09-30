-- EaWR hardpoint debris probe (#391). One function, EaWR_Debris_Probe_Service(), that the
-- capture mod's --hardpoint-debris-probe calls on every service of a space battle (0.5 s).
-- Lua 5.0 in FoC's GameScoring state; every game call goes through pcall, so a missing
-- function shows up as a DBP ERR advisor line instead of stopping the script.
--
-- It stages the retail reference for hardpoint deaths in a cycle the stills can catch at
-- any moment: a Nebulon_B_Frigate for the local player at the local Rebel station, the
-- camera pointed at it, its selected hardpoints destroyed one after the other, then
-- the next cycle. The all set keeps the original five-hardpoint/kill sequence.
-- The weapons and engines sets leave other hardpoints alive for a 30 s hold, then
-- despawn the ship. Every post_every services it posts the advisor line
--   DBP <set> c<cycle> <step> <live/dead> +<age> t<service>
-- (step: spawn, hp1..hp5, kill; age: services since that step; a service is 0.5 s of
-- game time), so a still says which deaths it follows and how long ago. The advisor
-- shows a line for 0.1 s per character, so the line is padded or cut to post_chars
-- characters, a little shorter on screen than the time to the next post, and no queue
-- builds up. Only the first post of a longer line (an ERR) is shown whole.

EaWR_Debris = {
	station = "Skirmish_Rebel_Star_Base_1",
	ship_type = "Nebulon_B_Frigate",
	hardpoints = {"HP_Nebulon_Weapon_FL", "HP_Nebulon_Weapon_FR", "HP_Nebulon_Weapon_BL",
	              "HP_Nebulon_Weapon_BR", "HP_Nebulon_Engines"},
	sets = {
		weapons = {"HP_Nebulon_Weapon_FL", "HP_Nebulon_Weapon_FR",
		           "HP_Nebulon_Weapon_BL", "HP_Nebulon_Weapon_BR"},
		engines = {"HP_Nebulon_Engines", "HP_Nebulon_Weapon_FL", "HP_Nebulon_Weapon_FR"},
	},
	set = EaWR_Debris_Set or "all",
	-- Well above each hardpoint's health and the frigate's shield, which may take the
	-- damage first.
	damage = 3000,
	start_wait = 10,     -- services before the first spawn (the battle settles)
	after_spawn = 6,     -- services from the spawn to the first hardpoint
	between = 4,         -- services between hardpoints
	before_kill = 16,    -- services from the last hardpoint to the ship's death
	live_hold = 60,      -- 30 s after the last selected hardpoint
	after_kill = 12,     -- services from the ship's death to the next spawn
	post_every = (EaWR_Debris_Set == "all") and 5 or 8,
	post_chars = (EaWR_Debris_Set == "all") and 24 or 34,
	post_wait = 1,
	service = 0,
	status = nil,        -- the last step's text
	status_service = 0,  -- the service it happened in
	wait = nil,
	cycle = 0,
	step = 0,            -- 0: spawn next; 1..5: that hardpoint next; 6: kill next
	ship = nil,
	ai_suspended = false,
}

function EaWR_Debris_Post(text)
	EaWR_Debris.status = text
	EaWR_Debris.status_service = EaWR_Debris.service
end

function EaWR_Debris_Error(text)
	EaWR_Debris_Post("ERR " .. text)
	pcall(Game_Message, "DBP ERR " .. text)
end

function EaWR_Debris_Dispose()
	local d = EaWR_Debris
	if not d.ship then
		return true
	end
	local disposed = true
	local ok, valid = pcall(function() return d.ship.Is_Valid() end)
	if not ok then
		EaWR_Debris_Error("ship-valid " .. string.sub(tostring(valid), 1, 60))
		disposed = false
	end
	if not ok or valid then
		local despawn_ok, result = pcall(function() return d.ship.Despawn() end)
		if not despawn_ok or result == false then
			EaWR_Debris_Error("despawn " .. string.sub(tostring(result), 1, 60))
			disposed = false
			local damage_ok, damage_result = pcall(function() return d.ship.Take_Damage(1000000) end)
			if not damage_ok or damage_result == false then
				EaWR_Debris_Error("cleanup-damage " .. string.sub(tostring(damage_result), 1, 60))
			end
		end
	end
	d.ship = nil
	return disposed
end

function EaWR_Debris_Reset()
	local d = EaWR_Debris
	EaWR_Debris_Dispose()
	d.service = 0
	d.status = nil
	d.status_service = 0
	d.post_wait = 1
	d.wait = nil
	d.cycle = 0
	d.step = 0
	d.ship = nil
	d.ai_suspended = false
end

function EaWR_Debris_Abort(reason)
	local d = EaWR_Debris
	EaWR_Debris_Error(reason)
	EaWR_Debris_Dispose()
	d.step = 0
	d.wait = d.after_kill
end

function EaWR_Debris_Show()
	local d = EaWR_Debris
	if not d.status then
		return
	end
	d.post_wait = d.post_wait - 1
	if d.post_wait > 0 then
		return
	end
	d.post_wait = d.post_every
	local live = ""
	if d.ship and d.set ~= "all" then
		local ok, valid = pcall(function() return d.ship.Is_Valid() end)
		live = (ok and valid) and " live" or " dead"
	end
	local line = "DBP " .. d.set .. " " .. d.status .. live .. " +" .. (d.service - d.status_service) .. " t" .. d.service
	if string.len(line) < d.post_chars then
		line = line .. string.rep(".", d.post_chars - string.len(line))
	elseif d.service - d.status_service >= d.post_every then
		line = string.sub(line, 1, d.post_chars)
	end
	pcall(Game_Message, line)
end

function EaWR_Debris_Spawn()
	local d = EaWR_Debris
	local ok, result = pcall(function()
		local player = Find_Player("local")
		local anchor = Find_First_Object(d.station)
		if not anchor then
			error("no " .. d.station)
		end
		local units = Spawn_Unit(Find_Object_Type(d.ship_type), anchor, player)
		if not units or not units[1] then
			error("spawn returned nothing")
		end
		return units[1]
	end)
	if not ok then
		EaWR_Debris_Post("ERR spawn " .. string.sub(tostring(result), 1, 60))
		return false
	end
	d.ship = result
	local camera_ok, camera_result = pcall(Point_Camera_At, d.ship)
	if not camera_ok or camera_result == false then
		EaWR_Debris_Abort("camera " .. string.sub(tostring(camera_result), 1, 60))
		return false
	end
	return true
end

function EaWR_Debris_Probe_Service()
	local d = EaWR_Debris
	if d.set ~= "all" and not d.sets[d.set] then
		EaWR_Debris_Post("ERR unknown set " .. tostring(d.set))
		return
	end
	local hardpoints = d.set == "all" and d.hardpoints or d.sets[d.set]
	d.service = d.service + 1
	if not d.ai_suspended then
		-- The AI player keeps its fleet home, so no fight crosses the staged deaths.
		local suspend_ok, suspend_result = pcall(Suspend_AI, 1)
		if not suspend_ok or suspend_result == false then
			EaWR_Debris_Error("suspend-ai " .. string.sub(tostring(suspend_result), 1, 60))
			return
		end
		d.ai_suspended = true
	end
	if d.wait == nil then
		d.wait = d.start_wait
	end
	if d.ship then
		local valid_ok, valid = pcall(function() return d.ship.Is_Valid() end)
		if not valid_ok then
			EaWR_Debris_Abort("ship-valid " .. string.sub(tostring(valid), 1, 60))
		elseif not valid then
			EaWR_Debris_Abort("early-death c" .. d.cycle)
		end
	end
	EaWR_Debris_Show()
	d.wait = d.wait - 1
	if d.wait > 0 then
		return
	end
	if d.step == 0 then
		d.cycle = d.cycle + 1
		if EaWR_Debris_Spawn() then
			EaWR_Debris_Post("c" .. d.cycle .. " spawn")
			d.step = 1
			d.wait = d.after_spawn
		else
			d.wait = d.after_kill
		end
	elseif d.step <= table.getn(hardpoints) then
		local name = hardpoints[d.step]
		local ok, message = pcall(function() return d.ship.Take_Damage(d.damage, name) end)
		if not ok or message == false then
			EaWR_Debris_Abort("hp" .. d.step .. " " .. string.sub(tostring(message), 1, 60))
			return
		end
		local camera_ok, camera_result = pcall(Point_Camera_At, d.ship)
		if not camera_ok or camera_result == false then
			EaWR_Debris_Abort("camera " .. string.sub(tostring(camera_result), 1, 60))
			return
		end
		EaWR_Debris_Post("c" .. d.cycle .. " hp" .. d.step)
		d.step = d.step + 1
		d.wait = d.step <= table.getn(hardpoints) and d.between or (d.set == "all" and d.before_kill or d.live_hold)
	else
		if d.set == "all" then
			local ok, result = pcall(function() return d.ship.Take_Damage(1000000) end)
			if not ok or result == false then
				EaWR_Debris_Abort("kill " .. string.sub(tostring(result), 1, 60))
				return
			end
			EaWR_Debris_Post("c" .. d.cycle .. " kill")
		else
			if EaWR_Debris_Dispose() then
				EaWR_Debris_Post("c" .. d.cycle .. " despawn")
			end
		end
		d.ship = nil
		d.step = 0
		d.wait = d.after_kill
	end
end
