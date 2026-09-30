#include "engine/input/ActionMap.hpp"
#include "engine/input/InputValue.hpp"
#include "engine/input/InputBinding.hpp"

#include <math/Vector2.hpp>
#include <algorithm>
#include <ranges>
#include <utility>

#include "engine/Logger.hpp"
#include "engine/input/InputBindingFactory.hpp"

using namespace N2Engine::Input;

using Vector2 = N2Engine::Math::Vector2;

InputAction::InputAction(std::string name)
    : _currentValue{false}, _inputActionName{std::move(name)} {}

void InputAction::Update()
{
    // Handle disabled state transition first
    HandleDisabledTransition();

    if (_disabled)
    {
        _wasDisabledLastFrame = true;
        return;
    }

    _wasDisabledLastFrame = false;
    _previousPhase = _currentPhase;

    // Store the previous value to detect changes
    const InputValue previousValue = _currentValue;

    // Read and combine all binding values
    _currentValue = CalculateCombinedValue();

    // Update phase based on current value
    UpdatePhase();

    // Check if the value changed significantly (for Vector2 inputs)
    bool valueChanged = false;

    // Compare Vector2 values with a small threshold to avoid floating point precision issues
    const Math::Vector2 prevVec = previousValue.asVector2();
    const Math::Vector2 currVec = _currentValue.asVector2();

    if (constexpr float threshold = 0.001f; std::abs(prevVec.x - currVec.x) > threshold ||
        std::abs(prevVec.y - currVec.y) > threshold)
    {
        valueChanged = true;
    }

    // Fire callback on phase transitions OR when value changes within the same phase

    if (bool shouldFireCallback = (_currentPhase != _previousPhase) || valueChanged)
    {
        _onStateChanged(*this);
    }
}

void InputAction::HandleDisabledTransition()
{
    // If we just became disabled and were active, cancel the action
    if (_disabled && !_wasDisabledLastFrame && IsActive())
    {
        _previousPhase = _currentPhase;
        _currentPhase = ActionPhase::Cancelled;
        _currentValue = false; // Reset value

        // Fire callback to notify of cancellation
        _onStateChanged(*this);
    }
    // If we just became enabled and were in cancelled state, reset to waiting
    else if (!_disabled && _wasDisabledLastFrame && _currentPhase == ActionPhase::Cancelled)
    {
        _previousPhase = _currentPhase;
        _currentPhase = ActionPhase::Waiting;

        // No callback needed for this transition - it's just cleanup
    }
}

void InputAction::SetDisabled(const bool disabled)
{
    _disabled = disabled;
    // Note: State transition handling is done in Update() via HandleDisabledTransition()
}

InputAction& InputAction::AddBinding(std::unique_ptr<InputBinding> binding)
{
    _bindings.push_back(std::move(binding));
    return *this;
}

InputValue InputAction::CalculateCombinedValue() const
{
    if (_bindings.empty())
    {
        return false;
    }

    // The action's value type follows its bindings: any 2D binding (stick, WASD composite) makes it a
    // Vector2 action, else any axis makes it a float, else it's a button. (It used to decide by
    // magnitude, so a small stick deflection became `true`, which reads as a full (1, 0) vector.)
    Vector2 combined(0, 0);
    bool anyVector = false;
    bool anyFloat = false;
    bool anyPressed = false;
    float strongestFloat = 0.0f;

    for (auto &binding : _bindings)
    {
        const InputValue bindingValue = binding->getValue();

        if (bindingValue.Is<Vector2>())
        {
            anyVector = true;
            combined += bindingValue.asVector2();
        }
        else if (bindingValue.Is<float>())
        {
            anyFloat = true;
            if (std::abs(bindingValue.asFloat()) > std::abs(strongestFloat))
            {
                strongestFloat = bindingValue.asFloat();
            }
        }
        else if (bindingValue.asBool())
        {
            anyPressed = true;
        }
    }

    if (anyVector)
    {
        // Several 2D bindings add up (e.g. keyboard and stick); keep within a unit circle
        return combined.Length() > 1.0f ? combined.Normalized() : combined;
    }
    if (anyFloat)
    {
        // A button bound alongside an axis counts as a full press
        return anyPressed && strongestFloat == 0.0f ? 1.0f : strongestFloat;
    }
    return anyPressed;
}

inline void InputAction::UpdatePhase()
{
    const bool hasInput = _currentValue.asBool();

    // Simple state machine for input phases
    switch (_currentPhase)
    {
    case ActionPhase::Waiting:
        if (hasInput)
        {
            _currentPhase = ActionPhase::Started;
        }
        break;

    case ActionPhase::Started:
        if (hasInput)
        {
            _currentPhase = ActionPhase::Performed;
        }
        else
        {
            _currentPhase = ActionPhase::Cancelled; // Input stopped before performing
        }
        break;

    case ActionPhase::Performed:
        if (!hasInput)
        {
            _currentPhase = ActionPhase::Waiting; // Input stopped after performing
        }
        // Stay in Performed while input continues
        break;

    case ActionPhase::Cancelled:
        if (!hasInput)
        {
            _currentPhase = ActionPhase::Waiting;
        }
        else
        {
            _currentPhase = ActionPhase::Started; // New input started
        }
        break;
    }
}

const std::string& InputAction::GetName() const
{
    return _inputActionName;
}

Vector2 InputAction::GetVector2Value() const
{
    return _currentValue.asVector2();
}

bool InputAction::GetBoolValue() const
{
    return _currentValue.asBool();
}

float InputAction::GetFloatValue() const
{
    return _currentValue.asFloat();
}

N2Engine::Base::EventHandler<InputAction&>& InputAction::GetOnStateChanged()
{
    return _onStateChanged;
}

void ActionMap::Update()
{
    if (disabled)
    {
        return;
    }

    // Snapshot: callbacks fired by an action can add, replace or remove actions, which would
    // invalidate iteration over the map (and replaced/removed ones are retired, not freed, meanwhile)
    std::vector<InputAction *> actions;
    for (const auto &inputAction : _inputActions | std::views::values)
    {
        if (inputAction)
        {
            actions.push_back(inputAction.get());
        }
    }

    // Set for the duration, cleared even if a handler throws; only the outermost update frees retired
    // actions (a handler may update this map again while the outer loop still holds them)
    struct UpdateScope
    {
        ActionMap &map;
        const bool outermost;

        explicit UpdateScope(ActionMap &m) : map(m), outermost(!m._updating) { map._updating = true; }

        ~UpdateScope()
        {
            if (outermost)
            {
                map._updating = false;
                map._retiredActions.clear();
            }
        }
    } scope{*this};

    for (InputAction *action : actions)
    {
        // Skip actions an earlier callback removed or replaced this frame. They're retired, not freed,
        // until the update ends, so reading the name is safe.
        const auto it = _inputActions.find(action->GetName());
        if (it != _inputActions.end() && it->second.get() == action)
        {
            action->Update();
        }
    }
}

ActionMap& ActionMap::AddInputAction(std::unique_ptr<InputAction> inputAction)
{
    if (const auto it = _inputActions.find(inputAction->GetName()); it != _inputActions.end() && _updating)
    {
        _retiredActions.push_back(std::move(it->second));
    }
    _inputActions.insert_or_assign(inputAction->GetName(), std::move(inputAction));
    return *this;
}

ActionMap& ActionMap::MakeInputAction(const std::string &actionName, const std::function<void(InputAction *)> &pAction)
{
    auto inputAction = std::make_unique<InputAction>(actionName);
    pAction(inputAction.get());
    AddInputAction(std::move(inputAction));
    return *this;
}

bool ActionMap::RemoveInputAction(const std::string &actionName)
{
    if (const auto it = _inputActions.find(actionName); it != _inputActions.end())
    {
        if (_updating)
        {
            _retiredActions.push_back(std::move(it->second)); // its Update may be on the stack
        }
        _inputActions.erase(it);
        return true;
    }
    return false;
}

InputAction& ActionMap::operator[](const std::string &mapName)
{
    return *_inputActions.at(mapName);
}

const InputAction& ActionMap::operator[](const std::string &mapName) const
{
    return *_inputActions.at(mapName);
}

nlohmann::json InputAction::Serialize() const
{
    nlohmann::json bindingsJson = nlohmann::json::array();
    for (const auto &binding : _bindings)
    {
        bindingsJson.push_back(binding->Serialize());
    }
    return {{"bindings", bindingsJson}};
}

std::expected<std::unique_ptr<InputAction>, ActionParseError> InputAction::Deserialize(
    const nlohmann::json &j, const std::string &actionName, GLFWwindow *window)
{
    if (!j.contains("bindings"))
    {
        return std::unexpected(ActionParseError::MissingBindings);
    }

    if (!j["bindings"].is_array())
    {
        return std::unexpected(ActionParseError::InvalidBindingsType);
    }

    auto action = std::make_unique<InputAction>(actionName);

    for (const auto &bindingJson : j["bindings"])
    {
        if (auto result = CreateBindingFromJson(window, bindingJson); result.has_value())
        {
            action->AddBinding(std::move(result.value()));
        }
        else
        {
            Logger::Warn(std::format("Invalid binding in action '{}' ({}): {}",
                                     actionName, BindingParseErrorToString(result.error()), bindingJson.dump()));
        }
    }

    return action;
}

nlohmann::json ActionMap::Serialize() const
{
    nlohmann::json actionsJson = nlohmann::json::object();
    for (const auto &[actionName, action] : _inputActions)
    {
        actionsJson[actionName] = action->Serialize();
    }
    return {
        {"disabled", disabled},
        {"actions", actionsJson}
    };
}

std::expected<std::unique_ptr<ActionMap>, ActionMapParseError> ActionMap::Deserialize(
    const nlohmann::json &j, const std::string &mapName, GLFWwindow *window)
{
    if (!j.contains("actions"))
    {
        return std::unexpected(ActionMapParseError::MissingActions);
    }

    if (!j["actions"].is_object())
    {
        return std::unexpected(ActionMapParseError::InvalidActionsType);
    }

    auto actionMap = std::make_unique<ActionMap>(mapName);
    actionMap->disabled = j.value("disabled", false);

    for (const auto &[actionName, actionJson] : j["actions"].items())
    {
        if (auto result = InputAction::Deserialize(actionJson, actionName, window); result.has_value())
        {
            actionMap->AddInputAction(std::move(result.value()));
        }
        else
        {
            Logger::Warn(std::format("Invalid action '{}' in ActionMap '{}': {}",
                                     actionName, mapName, ActionParseErrorToString(result.error())));
        }
    }

    return actionMap;
}

std::string N2Engine::Input::ActionParseErrorToString(const ActionParseError error)
{
    switch (error)
    {
    case ActionParseError::MissingBindings: return "missing 'bindings' field";
    case ActionParseError::InvalidBindingsType: return "'bindings' is not an array";
    }
    return "unknown error";
}

std::string N2Engine::Input::ActionMapParseErrorToString(const ActionMapParseError error)
{
    switch (error)
    {
    case ActionMapParseError::MissingActions: return "missing 'actions' field";
    case ActionMapParseError::InvalidActionsType: return "'actions' is not an object";
    }
    return "unknown error";
}
