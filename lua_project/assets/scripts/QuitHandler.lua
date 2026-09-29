-- Quits when the "Quit" action starts. Port of test_project/src/StandardInputHandler.cpp.

local QuitHandler = {}
QuitHandler.__index = QuitHandler

function QuitHandler:OnAttach()
    local controls = Input.LoadActionMap("Main Controls")
    if controls == nil then
        Debug.Warn("QuitHandler: 'Main Controls' action map not found")
        return
    end

    controls:Get("Quit"):Subscribe(function(action)
        if action:GetPhase() == ActionPhase.Started then
            Application.Quit()
        end
    end)
end

return QuitHandler
