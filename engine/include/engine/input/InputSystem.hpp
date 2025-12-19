#pragma once

#include <unordered_map>
#include <memory>
#include <string>
#include <functional>

#include <nlohmann/json.hpp>

namespace N2Engine
{
    class Window;

    namespace Input
    {
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

        public:
            explicit InputSystem(Window &window) : _window{window} {};
            ~InputSystem();

            ActionMap *LoadActionMap(const std::string &name);
            void AddActionMap(std::unique_ptr<ActionMap> &&actionMap);

            InputSystem &MakeActionMap(const std::string &name, const std::function<void(ActionMap *)> &pActionMap);

            ActionMap *GetCurActionMap() const;

            static std::vector<GamepadInfo> GetConnectedGamepads();

            void Update();

            nlohmann::json Serialize() const;
            bool Deserialize(const nlohmann::json& j);
        };
    }
}
