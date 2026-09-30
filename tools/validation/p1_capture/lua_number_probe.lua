-- EaWR Lua number probe (#376). One function, EaWR_Lua_Number_Probe(), that turns a
-- fixed list of number conversions into one line of text. Lua 5.0 with the base and
-- string libraries only, so the same file runs in FoC's GameScoring state (the capture
-- mod's --lua-number-probe, which shows the line in the battle with Game_Message) and
-- in our soft-float VM (lua_numeric_tests foc-probe, which compares it with the line
-- recorded from the retail game in tests/script/numeric/foc-retail-probe.txt).
--
-- Each case is {id, function, expected}; expected is the numeric profile's text
-- (docs/lua-numeric-profile.md). The line is
--   LNP1 OK n=26 A=2 B=4 ...          every case gave its expected text
--   LNP1 BAD=C,K n=26 A=2 B=4 ...     cases C and K did not
-- The game's own string comparison decides OK/BAD, so the verdict does not depend on
-- reading the digits off a screenshot. Values never contain spaces; a case with
-- several values joins them with commas. Keep ids and cases stable: a changed case
-- is a new probe version.

function EaWR_Lua_Number_Probe()
	-- PGBase.lua's Dirty_Floor and Simple_Mod: FoC has no math library.
	local function dirty_floor(value)
		return string.format("%d", value)
	end
	local function simple_mod(a, b)
		return a - b * dirty_floor(a / b)
	end
	local function join(...)
		local out = {}
		for i = 1, arg.n do
			out[i] = tostring(arg[i])
		end
		return table.concat(out, ",")
	end
	local zero = 0
	local cases = {
		{"A", function() return string.format("%.0f", 2.5) end, "2"},
		{"B", function() return string.format("%.0f", 3.5) end, "4"},
		{"C", function() return tostring(123456789012345) end, "1.2345678901234e+14"},
		{"D", function() return string.format("%.14g", 0.1 + 0.2) end, "0.3"},
		{"E", function() return tostring(1e15) end, "1e+15"},
		{"F", function() return tostring(-zero) end, "-0"},
		{"G", function() local p = 9007199254740992 return string.format("%.17g", p + 1) end, "9007199254740992"},
		{"H", function() return string.format("%.17g", 1 / 3) end, "0.33333333333333331"},
		{"I", function() return join(string.format("%.0f", 0.5), string.format("%.0f", 1.5), string.format("%.0f", -2.5)) end, "0,2,-2"},
		{"J", function() return join(string.format("%.1f", 0.25), string.format("%.2f", 1.125)) end, "0.2,1.12"},
		{"K", function() return string.format("%.20f", 0.1) end, "0.10000000000000000555"},
		{"L", function() return join(1 / zero, -1 / zero, zero / zero) end, "inf,-inf,-nan(ind)"},
		{"M", function() return join(dirty_floor(3e9), dirty_floor(-2.5), dirty_floor(2147483647.9)) end, "-2147483648,-2,2147483647"},
		{"N", function() return join(simple_mod(7, 3), simple_mod(-7, 3), simple_mod(7.5, 2), simple_mod(-1, 100)) end, "1,-1,1.5,-1"},
		{"O", function() return tostring(tonumber("0x10")) end, "16"},
		{"P", function() local ok = pcall(function() local a = 2 return a ^ 3 end) return ok and "pow" or "ERR" end, "ERR"},
		{"Q", function() return tostring(5e-324) end, "4.9406564584125e-324"},
		{"R", function() return join(string.format("%e", 12345.6789), string.format("%g", 1e-5)) end, "1.234568e+04,1e-05"},
		{"S", function() return tostring(123456789012345678) end, "1.2345678901235e+17"},
		{"T", function() return tostring(9007199254740993) end, "9.007199254741e+15"},
		{"U", function() return join(string.format("%.2f", 2.675), string.format("%.3f", 1.0005)) end, "2.67,1.000"},
		{"V", function() local n, last = 0 for i = 0, 1, 0.1 do n = n + 1 last = i end return join(n, string.format("%.17g", last)) end, "11,0.99999999999999989"},
		{"W", function() local s = 0 for i = 1, 100 do s = s + 1 / i end return string.format("%.17g", s) end, "5.1873775176396206"},
		{"X", function() return join(1e100, -1e-7) end, "1e+100,-1e-07"},
		{"Y", function() return join(string.format("%.15g", 0.1 * 3), string.format("%.17g", 1e23)) end, "0.3,9.9999999999999992e+22"},
		{"Z", function() return join(string.format("%x", 255.9), string.format("%05.1f", 1.25)) end, "ff,001.2"},
	}
	local values = {}
	local bad = {}
	for i = 1, table.getn(cases) do
		local case = cases[i]
		local ok, value = pcall(case[2])
		if not ok then
			value = "FAIL:" .. tostring(value)
		end
		values[i] = case[1] .. "=" .. value
		if value ~= case[3] then
			table.insert(bad, case[1])
		end
	end
	local verdict = "OK"
	if table.getn(bad) > 0 then
		verdict = "BAD=" .. table.concat(bad, ",")
	end
	return "LNP1 " .. verdict .. " n=" .. table.getn(cases) .. " " .. table.concat(values, " ")
end
