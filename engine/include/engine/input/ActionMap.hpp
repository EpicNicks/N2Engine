#pragma once

#include <unordered_map>
#include <string>
#include <memory>
#include <vector>
#include <functional>

#include "engine/base/EventHandler.hpp"
#include "engine/input/InputValue.hpp"

namespace N2Engine
{
    namespace Math
    {
        class Vector2;
    }

    namespace Input
    {
        enum class Key;
        enum class GamepadButton;
        enum class GamepadAxis;
        enum class MouseButton;
        class InputBinding;

        enum class ActionPhase
        {
            Waiting,   // No input
            Started,   // Action began
            Performed, // Action completed successfully
            Cancelled  // Action interrupted/failed
        };

        class InputAction
        {
        public:
            // Single callback that fires on any phase change

        private:
            Base::EventHandler<InputAction &> _onStateChanged;
            std::vector<std::unique_ptr<InputBinding>> _bindings;
            ActionPhase _currentPhase = ActionPhase::Waiting;
            ActionPhase _previousPhase = ActionPhase::Waiting;
            InputValue _currentValue;
            std::string _inputActionName;
            bool _disabled{false};
            bool _wasDisabledLastFrame{false};

        public:
            InputAction(const std::string name);
            ~InputAction() = default;

            InputAction(const InputAction &) = delete;
            InputAction &operator=(const InputAction &) = delete;

            InputAction(InputAction &&) = default;
            InputAction &operator=(InputAction &&) = default;

            void Update();

            // Add bindings
            InputAction &AddBinding(std::unique_ptr<InputBinding> binding);

            // Disabled property with proper state handling
            bool GetDisabled() const { return _disabled; }
            void SetDisabled(bool disabled);

            Base::EventHandler<InputAction &> &GetOnStateChanged();

            // Public API for querying current state
            ActionPhase GetPhase() const { return _currentPhase; }
            Math::Vector2 GetVector2Value() const;
            bool GetBoolValue() const;
            float GetFloatValue() const;
            const std::string &GetName() const;

            // Convenience methods for checking phase transitions
            bool WasStarted() const { return _currentPhase == ActionPhase::Started && _previousPhase == ActionPhase::Waiting; }
            bool WasPerformed() const { return _currentPhase == ActionPhase::Performed; }
            bool WasCancelled() const { return _currentPhase == ActionPhase::Cancelled; }
            bool IsActive() const { return _currentPhase == ActionPhase::Started || _currentPhase == ActionPhase::Performed; }

        private:
            InputValue CalculateCombinedValue();
            void UpdatePhase();
            void HandleDisabledTransition();
        };

        class ActionMap
        {
        private:
            std::unordered_map<std::string, std::unique_ptr<InputAction>> _inputActions;

        public:
            const std::string name;
            bool disabled{false};

            ActionMap(std::string mapName) : name(std::move(mapName)) {}
            ActionMap &AddInputAction(std::unique_ptr<InputAction> inputAction);
            ActionMap &MakeInputAction(const std::string name, std::function<void(InputAction *)> pAction);
            bool RemoveInputAction(const std::string &name);
            void Update();

            InputAction &operator[](std::string mapName);
            const InputAction &operator[](std::string mapName) const;
        };
    }
}