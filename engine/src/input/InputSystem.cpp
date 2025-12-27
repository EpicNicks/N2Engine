#include "engine/input/InputSystem.hpp"
#include "engine/input/ActionMap.hpp"
#include "engine/input/InputBinding.hpp"
#include "engine/input/InputBindingFactory.hpp"
#include "engine/Logger.hpp"

#include "engine/Window.hpp"

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include "engine/input/Mouse.hpp"

using namespace N2Engine::Input;

InputSystem::InputSystem(Window &window)
    : _window{window}
{
    _mouse = std::make_unique<Mouse>(_window._window);
    glfwSetWindowUserPointer(_window._window, _mouse.get());
}

InputSystem::~InputSystem() = default;

void InputSystem::AddActionMap(std::unique_ptr<ActionMap> &&actionMap)
{
    const std::string mapName = actionMap->name; // Store name before moving
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
        _curActionMapName = it->first;
        return GetCurActionMap();
    }
    return nullptr;
}

ActionMap* InputSystem::GetActionMap(const std::string &name)
{
    if (const auto it = _actionMaps.find(name); it != _actionMaps.end())
    {
        _curActionMapName = it->first;
        return GetCurActionMap();
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
                    Logger::Warn("Malformed binding: " + bindingJson["name"].get<std::string>());
                }
                // Silently skip invalid bindings
            }

            actionMap->AddInputAction(std::move(inputAction));
        }

        newMaps.insert_or_assign(mapName, std::move(actionMap));
    }

    // Commit changes
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
