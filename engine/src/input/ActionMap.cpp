#include "engine/input/ActionMap.hpp"
#include "engine/input/InputValue.hpp"
#include "engine/input/InputBinding.hpp"

#include <math/Vector2.hpp>

using namespace N2Engine::Input;

using Vector2 = N2Engine::Math::Vector2;

InputAction::InputAction(const std::string name)
    : _inputActionName{name}, _currentValue{false}
{
}

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
    InputValue previousValue = _currentValue;

    // Read and combine all binding values
    _currentValue = CalculateCombinedValue();

    // Update phase based on current value
    UpdatePhase();

    // Check if the value changed significantly (for Vector2 inputs)
    bool valueChanged = false;

    // Compare Vector2 values with a small threshold to avoid floating point precision issues
    Math::Vector2 prevVec = previousValue.asVector2();
    Math::Vector2 currVec = _currentValue.asVector2();
    float threshold = 0.001f; // Small threshold for floating point comparison

    if (std::abs(prevVec.x - currVec.x) > threshold ||
        std::abs(prevVec.y - currVec.y) > threshold)
    {
        valueChanged = true;
    }

    // Fire callback on phase transitions OR when value changes within the same phase
    bool shouldFireCallback = (_currentPhase != _previousPhase) || valueChanged;

    if (shouldFireCallback)
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

void InputAction::SetDisabled(bool disabled)
{
    _disabled = disabled;
    // Note: State transition handling is done in Update() via HandleDisabledTransition()
}

InputAction &InputAction::AddBinding(std::unique_ptr<InputBinding> binding)
{
    _bindings.push_back(std::move(binding));
    return *this;
}

InputValue InputAction::CalculateCombinedValue()
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
        float floatVal = std::abs(bindingValue.asFloat());
        if (floatVal > std::abs(maxFloat))
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
    float magnitude = _currentValue.asVector2().Magnitude();
    bool hasInput = _currentValue.asBool();

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

const std::string &InputAction::GetName() const
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

N2Engine::Base::EventHandler<InputAction &> &InputAction::GetOnStateChanged()
{
    return _onStateChanged;
}

void ActionMap::Update()
{
    if (disabled)
    {
        return;
    }

    for (auto &[actionName, inputAction] : _inputActions)
    {
        if (inputAction)
        {
            inputAction->Update();
        }
    }
}

ActionMap &ActionMap::AddInputAction(std::unique_ptr<InputAction> inputAction)
{
    _inputActions.insert_or_assign(inputAction->GetName(), std::move(inputAction));
    return *this;
}

ActionMap &ActionMap::MakeInputAction(const std::string name, std::function<void(InputAction *)> pAction)
{
    auto inputAction = std::make_unique<InputAction>(name);
    pAction(inputAction.get());
    AddInputAction(std::move(inputAction));
    return *this;
}

bool ActionMap::RemoveInputAction(const std::string &name)
{
    if (auto it = _inputActions.find(name); it != _inputActions.end())
    {
        _inputActions.erase(it);
        return true;
    }
    return false;
}

InputAction &ActionMap::operator[](std::string mapName)
{
    return *(_inputActions.at(mapName));
}

const InputAction &ActionMap::operator[](std::string mapName) const
{
    return *(_inputActions.at(mapName));
}