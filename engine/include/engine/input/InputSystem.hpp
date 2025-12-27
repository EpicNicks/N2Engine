#pragma once

#include <unordered_map>
#include <memory>
#include <string>
#include <functional>

#include <nlohmann/json.hpp>

struct GLFWwindow;

namespace N2Engine
{
    class Window;
}

namespace N2Engine::Input
{
    class Mouse;
    class ActionMap;

    struct GamepadInfo
    {
        std::string name;
        int gamepadId;
    };

    class InputSystem
    {
    private:
        Window &_window;
        std::unordered_map<std::string, std::unique_ptr<ActionMap>> _actionMaps;
        std::string _curActionMapName;
        std::unique_ptr<Mouse> _mouse;

    public:
        explicit InputSystem(Window &window);
        ~InputSystem();

        ActionMap* LoadActionMap(const std::string &name);
        ActionMap* GetActionMap(const std::string &name);
        void AddActionMap(std::unique_ptr<ActionMap> &&actionMap);
        InputSystem& MakeActionMap(const std::string &name, const std::function<void(ActionMap *)> &pActionMap);
        [[nodiscard]] ActionMap* GetCurActionMap() const;

        static std::vector<GamepadInfo> GetConnectedGamepads();
        [[nodiscard]] Mouse* GetMouse() const { return _mouse.get(); }

        void Update();

        [[nodiscard]] nlohmann::json Serialize() const;
        bool Deserialize(const nlohmann::json &j);
    };
}
