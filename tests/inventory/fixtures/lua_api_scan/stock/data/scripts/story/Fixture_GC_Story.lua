require("Fixture_Lib")

function Definitions()
	Fixture_Start()
end

function Fixture_Start()
	local player = Find_Player("Rebel")
	local planets = Find_Path(player, "Fixture_Alpha", "Fixture_Beta")
	Free_Local_Helper()
	local roll = GameRandom.Free_Random(1, 6)
	return planets, player, roll
end

function Free_Local_Helper()
	return Lib_Helper()
end

function Story_Handle_Event(event)
	local unit = Find_First_Object("Fixture_Ship")
	if unit:Is_In_Garrison() then
		unit:Garrison()
	end
	Game_Message("TEXT_FIXTURE")
end

Create_Thread("Fixture_Start")
