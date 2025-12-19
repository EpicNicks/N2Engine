#include "engine/input/ActionMap.hpp"
#include "engine/input/InputValue.hpp"
#include "engine/input/InputBinding.hpp"

#include <math/Vector2.hpp>
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

    // For Vector2 actions - combine all bindings additively
    Vector2 combined(0, 0);
    bool anyBoolTrue = false;
    float maxFloat = 0.0f;

    for (auto &binding : _bindings)
    {
        InputValue bindingValue = binding->getValue();

        // Combine as Vector2 (covers most cases)
        combined += bindingValue.asVector2();

        // Track bool values (for button-like actions)
        if (bindingValue.asBool())
        {
            anyBoolTrue = true;
        }

        // Track float values
        if (const float floatVal = std::abs(bindingValue.asFloat()); floatVal > std::abs(maxFloat))
        {
            maxFloat = bindingValue.asFloat();
        }
    }

    // Normalize combined vector if it exceeds unit length (for stick-like behavior)
    if (combined.Magnitude() > 1.0f)
    {
        combined = combined.Normalized();
    }

    // Return the most appropriate type
    // If we have significant vector movement, return that
    if (combined.Magnitude() > 0.1f)
    {
        return combined;
    }
    // If we have any bool input, return that
    else if (anyBoolTrue)
    {
        return true;
    }
    // Otherwise return the float value
    else
    {
        return maxFloat;
    }
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

    for (auto &inputAction : _inputActions | std::views::values)
    {
        if (inputAction)
        {
            inputAction->Update();
        }
    }
}

ActionMap& ActionMap::AddInputAction(std::unique_ptr<InputAction> inputAction)
{
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
        Logger::Warn("Invalid binding found in action: " + actionName);
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
        Logger::Warn("Invalid binding found in ActionMap: " + mapName);
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
