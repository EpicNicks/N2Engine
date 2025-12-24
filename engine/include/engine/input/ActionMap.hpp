#pragma once

#include <expected>
#include <unordered_map>
#include <string>
#include <memory>
#include <vector>
#include <functional>

#include "engine/base/EventHandler.hpp"
#include "engine/input/InputValue.hpp"
#include "nlohmann/json.hpp"

class GLFWwindow;

namespace N2Engine::Math
{
    class Vector2;
}

namespace N2Engine::Input
{
    enum class Key;
    enum class GamepadButton;
    enum class GamepadAxis;
    enum class MouseButton;
    class InputBinding;


    enum class ActionPhase
    {
        Waiting, // No input
        Started, // Action began
        Performed, // Action completed successfully
        Cancelled // Action interrupted/failed
    };

    enum class ActionParseError
    {
        MissingBindings,
        InvalidBindingsType
    };
    std::string ActionParseErrorToString(ActionParseError error);


    enum class ActionMapParseError
    {
        MissingActions,
        InvalidActionsType
    };
    std::string ActionMapParseErrorToString(ActionMapParseError error);


    class InputAction
    {
    private:
        Base::EventHandler<InputAction&> _onStateChanged;
        std::vector<std::unique_ptr<InputBinding>> _bindings;
        ActionPhase _currentPhase = ActionPhase::Waiting;
        ActionPhase _previousPhase = ActionPhase::Waiting;
        InputValue _currentValue;
        std::string _inputActionName;
        bool _disabled{false};
        bool _wasDisabledLastFrame{false};

    public:
        explicit InputAction(std::string name);
        ~InputAction() = default;

        InputAction(const InputAction &) = delete;
        InputAction& operator=(const InputAction &) = delete;

        InputAction(InputAction &&) = default;
        InputAction& operator=(InputAction &&) = default;

        void Update();

        // Add bindings
        InputAction& AddBinding(std::unique_ptr<InputBinding> binding);

        // Disabled property with proper state handling
        [[nodiscard]] bool GetDisabled() const { return _disabled; }
        void SetDisabled(bool disabled);

        Base::EventHandler<InputAction&>& GetOnStateChanged();

        // Public API for querying current state
        [[nodiscard]] ActionPhase GetPhase() const { return _currentPhase; }
        [[nodiscard]] Math::Vector2 GetVector2Value() const;
        [[nodiscard]] bool GetBoolValue() const;
        [[nodiscard]] float GetFloatValue() const;
        [[nodiscard]] const std::string& GetName() const;

        // Convenience methods for checking phase transitions
        [[nodiscard]] bool WasStarted() const
        {
            return _currentPhase == ActionPhase::Started && _previousPhase == ActionPhase::Waiting;
        }

        [[nodiscard]] bool WasPerformed() const { return _currentPhase == ActionPhase::Performed; }
        [[nodiscard]] bool WasCancelled() const { return _currentPhase == ActionPhase::Cancelled; }

        [[nodiscard]] bool IsActive() const
        {
            return _currentPhase == ActionPhase::Started || _currentPhase == ActionPhase::Performed;
        }

        [[nodiscard]] nlohmann::json Serialize() const;
        static std::expected<std::unique_ptr<InputAction>, ActionParseError> Deserialize(
            const nlohmann::json &j,
            const std::string &actionName,
            GLFWwindow *window
        );

    private:
        [[nodiscard]] InputValue CalculateCombinedValue() const;
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

        explicit ActionMap(std::string mapName) : name(std::move(mapName)) {}
        ActionMap& AddInputAction(std::unique_ptr<InputAction> inputAction);
        ActionMap& MakeInputAction(const std::string &actionName, const std::function<void(InputAction *)> &pAction);
        bool RemoveInputAction(const std::string &actionName);
        void Update();

        InputAction& operator[](const std::string &mapName);
        const InputAction& operator[](const std::string &mapName) const;

        [[nodiscard]] nlohmann::json Serialize() const;
        static std::expected<std::unique_ptr<ActionMap>, ActionMapParseError> Deserialize(
            const nlohmann::json &j,
            const std::string &mapName,
            GLFWwindow *window
        );
    };
}
