#include "engine/input/InputSystem.hpp"
#include "engine/input/ActionMap.hpp"
#include "engine/input/InputBinding.hpp"
#include "engine/input/InputBindingFactory.hpp"
#include "engine/Logger.hpp"

#include "engine/Window.hpp"

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include "engine/input/Mouse.hpp"

#include <ranges>

using namespace N2Engine::Input;

// Set for the duration, cleared even if a handler throws; only the outermost scope frees retired maps
// (a handler may replace a map whose Update or cancel pass is still on the stack)
struct InputSystem::UpdatingScope
{
    InputSystem &input;
    const bool outermost;

    explicit UpdatingScope(InputSystem &i) : input(i), outermost(!i._updating) { input._updating = true; }

    ~UpdatingScope()
    {
        if (outermost)
        {
            input._updating = false;
            input._retiredMaps.clear();
        }
    }

    UpdatingScope(const UpdatingScope &) = delete;
    UpdatingScope &operator=(const UpdatingScope &) = delete;
};

InputSystem::InputSystem(Window &window)
    : _window{window}
{
    // The window's user pointer stays the Window: its resize callback needs it (this used to overwrite
    // it with the Mouse, so resizing called Window::OnWindowResize on a Mouse). The Mouse's scroll
    // callback reaches it through the Window instead.
    _mouse = std::make_unique<Mouse>(_window._window);
}

InputSystem::~InputSystem() = default;

ActionMap* InputSystem::CreateActionMapFromJson(const std::string &name, const nlohmann::json &j)
{
    auto result = ActionMap::Deserialize(j, name, _window._window);
    if (!result.has_value())
    {
        Logger::Error(std::format("Failed to create action map '{}': {}",
                                  name, ActionMapParseErrorToString(result.error())));
        return nullptr;
    }

    AddActionMap(std::move(result.value()));
    return GetActionMap(name);
}

void InputSystem::AddActionMap(std::unique_ptr<ActionMap> &&actionMap)
{
    const std::string mapName = actionMap->name; // Store name before moving
    if (const auto it = _actionMaps.find(mapName); it != _actionMaps.end() && _updating)
    {
        // Replaced from an input callback: that map's Update may be on the stack, so keep it alive
        // until this frame's update finishes
        _retiredMaps.push_back(std::move(it->second));
    }
    _actionMaps.insert_or_assign(mapName, std::move(actionMap));

    // If this is the first map, make it current
    if (_curActionMapName.empty())
    {
        _curActionMapName = mapName;
    }
}

InputSystem& InputSystem::MakeActionMap(const std::string &name, const std::function<void(ActionMap *)> &pActionMap)
{
    auto actionMap = std::make_unique<ActionMap>(name);
    pActionMap(actionMap.get());
    AddActionMap(std::move(actionMap));
    return *this;
}

ActionMap* InputSystem::LoadActionMap(const std::string &name)
{
    if (_curActionMapName == name)
    {
        return GetCurActionMap();
    }
    if (const auto it = _actionMaps.find(name); it != _actionMaps.end())
    {
        ActionMap *previous = GetCurActionMap();
        _curActionMapName = it->first;
        if (previous)
        {
            // Leaving a map releases what was held in it: it isn't updated any more, so its actions would
            // otherwise stay Started/Performed with no callback until it was loaded again. Its callbacks may
            // replace maps, which are then retired until this pass ends rather than freed under it.
            UpdatingScope scope{*this};
            previous->CancelActiveActions();
        }
        return GetCurActionMap();
    }
    return nullptr;
}

ActionMap* InputSystem::GetActionMap(const std::string &name)
{
    // Only looks up; LoadActionMap is what switches the active map (this used to switch too)
    if (const auto it = _actionMaps.find(name); it != _actionMaps.end())
    {
        return it->second.get();
    }
    return nullptr;
}

ActionMap* InputSystem::GetCurActionMap() const
{
    if (!_curActionMapName.empty() && _actionMaps.contains(_curActionMapName))
    {
        return _actionMaps.at(_curActionMapName).get();
    }
    return nullptr;
}

std::vector<GamepadInfo> InputSystem::GetConnectedGamepads()
{
    std::vector<GamepadInfo> result;
    for (int i = GLFW_JOYSTICK_1; i <= GLFW_JOYSTICK_LAST; ++i)
    {
        if (glfwJoystickPresent(i))
        {
            if (glfwJoystickIsGamepad(i))
            {
                const char *name = glfwGetGamepadName(i);
                result.push_back(name != nullptr ? GamepadInfo{name, i} : GamepadInfo{"Nameless Gamepad", i});
            }
            else
            {
                const char *name = glfwGetJoystickName(i);
                result.push_back(name != nullptr
                                     ? GamepadInfo{"Unrecognized Gamepad Mapping: " + std::string(name), i}
                                     : GamepadInfo{"Namless Unrecognized Gamepad Mapping", i});
            }
        }
    }
    return result;
}

void InputSystem::Update()
{
    _mouse->Update();

    UpdatingScope scope{*this};

    if (const auto it = _actionMaps.find(_curActionMapName); it != _actionMaps.end())
    {
        it->second->Update();
    }
}

nlohmann::json InputSystem::Serialize() const
{
    nlohmann::json mapsJson;
    for (const auto &[mapName, actionMap] : _actionMaps)
    {
        mapsJson[mapName] = actionMap->Serialize();
    }
    return {{"actionMaps", mapsJson}};
}

bool InputSystem::Deserialize(const nlohmann::json &j)
{
    if (!j.contains("actionMaps") || !j["actionMaps"].is_object())
    {
        return false;
    }

    // Build new maps first, only commit if successful
    std::unordered_map<std::string, std::unique_ptr<ActionMap>> newMaps;

    for (const auto &[mapName, mapJson] : j["actionMaps"].items())
    {
        if (!mapJson.contains("actions") || !mapJson["actions"].is_object())
        {
            continue; // Skip malformed action maps
        }
        if (!ActionMap::HasValidDisabledField(mapJson))
        {
            Logger::Warn("Malformed action map (\"disabled\" is not a boolean), skipped: " + mapName);
            continue;
        }

        auto actionMap = std::make_unique<ActionMap>(mapName);
        actionMap->disabled = mapJson.value("disabled", false);

        for (const auto &[actionName, actionJson] : mapJson["actions"].items())
        {
            if (!actionJson.contains("bindings") || !actionJson["bindings"].is_array())
            {
                Logger::Warn("Malformed actions list detected in map: " + mapName);
                continue; // Skip malformed actions
            }

            auto inputAction = std::make_unique<InputAction>(actionName);

            for (const auto &bindingJson : actionJson["bindings"])
            {
                if (auto binding = CreateBindingFromJson(_window._window, bindingJson); binding.has_value())
                {
                    inputAction->AddBinding(std::move(binding.value()));
                }
                else
                {
                    Logger::Warn("Malformed binding: " + bindingJson.dump());
                }
                // Silently skip invalid bindings
            }

            actionMap->AddInputAction(std::move(inputAction));
        }

        newMaps.insert_or_assign(mapName, std::move(actionMap));
    }

    // Commit changes. Reloading from an action's callback must not free the map whose Update is on
    // the stack, so while updating the old maps are retired instead (freed when the update ends).
    if (_updating)
    {
        for (auto &map : _actionMaps | std::views::values)
        {
            _retiredMaps.push_back(std::move(map));
        }
    }
    _actionMaps = std::move(newMaps);

    // Set first map as current if we had one before or pick any
    if (!_actionMaps.empty())
    {
        if (!_actionMaps.contains(_curActionMapName))
        {
            _curActionMapName = _actionMaps.begin()->first;
        }
    }
    else
    {
        _curActionMapName.clear();
    }

    return true;
}
