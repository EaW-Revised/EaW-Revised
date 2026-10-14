-- Private benchmark overlay, appended through the capture mod's Lua-input route.
-- Requires the existing melee staging helpers. Inputs come from our native replay.
-- It stages once, records placement error, and never silently substitutes an order.
EaWR_Stage.start_wait = EaWR_Melee_Start_Seconds
EaWR_Stage.wait_for = EaWR_Melee_Start_Seconds

function EaWR_Stage_Melee_Camera()
    -- Keep the camera at the benchmark centre throughout the measurement.
    if not EaWR_Stage.melee_fixed_camera then
        if not EaWR_Stage_Call("camera-centre", Point_Camera_At, Create_Position(0, 0, 0)) then return end
        EaWR_Stage.melee_fixed_camera = true
    end
end

function EaWR_Melee_Count_Line(now)
    local d = EaWR_Stage
    local rs, rq = EaWR_Stage_Melee_Count(d.melee_sides[1])
    local es, eq = EaWR_Stage_Melee_Count(d.melee_sides[2])
    local ok, game = pcall(GetCurrentTime)
    local game_text = ok and type(game) == "number" and string.format("%.1f", game) or "unavailable"
    d.line = "ML " .. d.phase .. " +" .. string.format("%.1f", now - d.phase_start) ..
        " R" .. rs .. "/" .. rq .. " E" .. es .. "/" .. eq ..
        " err" .. string.format("%.1f", d.melee_max_error or 0) .. " L" ..
        tostring(LastService) .. " g" .. game_text
    return rs + rq, es + eq
end

function EaWR_Stage_Melee()
    local d = EaWR_Stage
    local now = EaWR_Stage_Now()
    if d.err then d.line = d.err return end
    if d.phase == "wait" then
        EaWR_Stage_Wait_Line("ML", now - d.phase_start)
        if now - d.phase_start < d.wait_for then return end
        d.melee_sides = {{}, {}}
        d.melee_by_id = {}
        d.melee_max_error = 0
        local side_for = {}
        local nside = 0
        for i = 1, table.getn(EaWR_Melee_Input.units) do
            local row = EaWR_Melee_Input.units[i]
            local player_row = EaWR_Melee_Input.players[row.owner]
            local player = Find_Player(player_row.faction)
            if not player then EaWR_Stage_Error("missing fixture owner") return end
            if not side_for[row.owner] then
                nside = nside + 1
                if nside > 2 then EaWR_Stage_Error("expected two fixture sides") return end
                side_for[row.owner] = nside
            end
            local position = Create_Position(unpack(row.position))
            local units = Spawn_Unit(Find_Object_Type(row.type), position, player)
            if not units or not units[1] then EaWR_Stage_Error("spawn " .. row.type) return end
            local object = units[1]
            -- Retail may choose a nearby collision-free position. Measure that
            -- deviation instead of accepting the old 500-unit teleport tolerance.
            if not EaWR_Stage_Call("teleport", function() return object.Teleport(position) end) then return end
            local x, y, z = EaWR_Stage_XY(object)
            if not x then EaWR_Stage_Error("unobservable fixture position") return end
            local dx, dy, dz = x - row.position[1], y - row.position[2], z - row.position[3]
            local deviation = EaWR_Stage_Sqrt(dx * dx + dy * dy + dz * dz)
            if deviation > d.melee_max_error then d.melee_max_error = deviation end
            local entry = {object = object, squadron = table.getn(row.members) > 0,
                           x = row.position[1], y = row.position[2]}
            d.melee_by_id[row.id] = entry
            table.insert(d.melee_sides[side_for[row.owner]], entry)
        end
        if nside ~= 2 then EaWR_Stage_Error("expected two fixture sides") return end
        d.cycle = 1
        d.melee_form = "native-replay"
        -- Rotation and per-craft formation are retained in fixture.json for an
        -- independent audit; this public spawn/teleport route does not set them.
        EaWR_Stage_Melee_Camera()
        EaWR_Stage_Set_Phase("settle")
        return
    end
    local rebels, empire = EaWR_Melee_Count_Line(now)
    if d.phase == "settle" then
        if now - d.phase_start < d.melee_settle then return end
        for c = 1, table.getn(EaWR_Melee_Input.commands) do
            local command = EaWR_Melee_Input.commands[c]
            for i = 1, table.getn(command.units) do
                local entry = d.melee_by_id[command.units[i]]
                if command.kind == "attack" then
                    local target = d.melee_by_id[command.target]
                    if not EaWR_Stage_Call("native-attack", function()
                        return entry.object.Attack_Target(target.object)
                    end) then return end
                else
                    if not EaWR_Stage_Call("native-attack-move", function()
                        return entry.object.Attack_Move(Create_Position(unpack(command.position)))
                    end) then return end
                end
            end
        end
        EaWR_Stage_Set_Phase("fight")
    elseif d.phase == "fight" and (rebels == 0 or empire == 0 or now - d.phase_start >= d.melee_fight) then
        -- Retain survivors for the final audit, with no new cycle contaminating it.
        EaWR_Stage_Set_Phase("done")
    end
end
