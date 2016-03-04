_g_ScriptTitle = "Jumpbot"

function OnScriptInit()
	return _system.Import(_g_ScriptUID, "include/baselib.lua")
end

y = -1
function OnTick()

	if(IsGrounded() == true) then -- jump if we hit the ground
		y = GetPlayerY()
		Jump()
	else
		ResetJump()
		if(GetPlayerY() > y+12) then -- do doublejump if we dropped deeper than we jumped high
			Jump()
		end
	end
end

RegisterEvent("OnTick", "OnTick")
