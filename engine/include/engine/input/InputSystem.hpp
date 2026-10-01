#pragma once

#include <unordered_map>
#include <memory>
#include <string>
#include <functional>
#include <vector>

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
        // Maps replaced while callbacks run are freed after the update, not under their own Update
        bool _updating = false;
        std::vector<std::unique_ptr<ActionMap>> _retiredMaps;
        // Sets _updating while action callbacks run (Update, or cancelling a map on LoadActionMap)
        struct UpdatingScope;

    public:
        explicit InputSystem(Window &window);
        ~InputSystem();

        /// Makes the named map current. Switching to another map cancels the previous map's active actions
        /// (ActionMap::CancelActiveActions), so their subscribers see the release.
        ActionMap* LoadActionMap(const std::string &name);
        ActionMap* GetActionMap(const std::string &name);
        void AddActionMap(std::unique_ptr<ActionMap> &&actionMap);
        InputSystem& MakeActionMap(const std::string &name, const std::function<void(ActionMap *)> &pActionMap);
        /// Builds a map from the JSON ActionMap::Deserialize accepts ({"actions": {...}}), adding or replacing it.
        /// @returns the new map, or nullptr if the JSON is malformed
        ActionMap* CreateActionMapFromJson(const std::string &name, const nlohmann::json &j);
        [[nodiscard]] ActionMap* GetCurActionMap() const;

        static std::vector<GamepadInfo> GetConnectedGamepads();
        [[nodiscard]] Mouse* GetMouse() const { return _mouse.get(); }

        void Update();

        [[nodiscard]] nlohmann::json Serialize() const;
        bool Deserialize(const nlohmann::json &j);
    };
}
