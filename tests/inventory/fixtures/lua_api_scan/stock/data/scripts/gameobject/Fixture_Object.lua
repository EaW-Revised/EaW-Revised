function Definitions()
	Define_State("State_Init", State_Init)
end

function State_Init(message)
	if Object.Get_Owner():Is_Human() then
		Object.Play_SFX_Event("FIXTURE")
	end
end
