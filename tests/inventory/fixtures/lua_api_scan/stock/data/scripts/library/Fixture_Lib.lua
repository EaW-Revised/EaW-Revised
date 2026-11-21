function Lib_Helper()
	local all = Find_All_Objects_Of_Type("Fixture_Ship")
	return all
end

Fixture_Table = {}
function Fixture_Table.Method()
	return Find_Nearest(nil, "x")
end
