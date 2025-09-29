#include "engine/input/InputSystem.hpp"
#include "engine/input/ActionMap.hpp"
#include "engine/input/InputBinding.hpp"

using namespace N2Engine::Input;

InputSystem::~InputSystem() = default;

void InputSystem::AddActionMap(std::unique_ptr<ActionMap> &&actionMap)
{
    std::string mapName = actionMap->name; // Store name before moving
    _actionMaps.insert_or_assign(mapName, std::move(actionMap));

    // If this is the first map, make it current
    if (_curActionMapName.empty())
    {
        _curActionMapName = mapName;
    }
}

InputSystem &InputSystem::MakeActionMap(const std::string name, std::function<void(ActionMap *)> pActionMap)
{
    auto actionMap = std::make_unique<ActionMap>(name);
    pActionMap(actionMap.get());
    AddActionMap(std::move(actionMap));
    return *this;
}

ActionMap *InputSystem::LoadActionMap(std::string name)
{
    if (_curActionMapName == name)
    {
        return GetCurActionMap();
    }
    if (auto it = _actionMaps.find(name); it != _actionMaps.end())
    {
        _curActionMapName = it->first;
        return GetCurActionMap();
    }
    return nullptr;
}

ActionMap *InputSystem::GetCurActionMap()
{
    if (!_curActionMapName.empty() && _actionMaps.count(_curActionMapName) > 0)
    {
        return _actionMaps.at(_curActionMapName).get();
    }
    return nullptr;
}

void InputSystem::Update()
{
    if (auto it = _actionMaps.find(_curActionMapName); it != _actionMaps.end())
    {
        it->second->Update();
    }
}