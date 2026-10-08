#include <gtest/gtest.h>

#include <memory>

#include <math/Vector2.hpp>

#include "engine/input/ActionMap.hpp"
#include "engine/input/InputBinding.hpp"
#include "engine/input/InputTypes.hpp"
#include "engine/input/InputValue.hpp"
#include "engine/input/KeySource.hpp"

// Input::KeySource (#82, E9): the seam the editor's play child feeds keys and mouse buttons through, as it has no
// window to poll. Bindings with no window at all read nothing by default, so a binding that reads a pressed key here
// can only have asked the source.

using namespace N2Engine;
using namespace N2Engine::Input;

namespace
{
    /// Installs a source for a test and takes it away again, whatever the test does
    class SourceScope
    {
    public:
        explicit SourceScope(const KeySource *source) : _previous(KeySource::Set(source)) {}
        ~SourceScope() { KeySource::Set(_previous); }
        SourceScope(const SourceScope &) = delete;
        SourceScope &operator=(const SourceScope &) = delete;

    private:
        const KeySource *_previous;
    };

    GLFWwindow *NoWindow() { return nullptr; }
}

TEST(InjectedKeysTest, AKeyIsDownFromItsPressUntilItsRelease)
{
    InjectedKeys keys;
    EXPECT_FALSE(keys.IsKeyDown(Key::W));
    EXPECT_FALSE(keys.AnyDown());

    keys.SetKey(Key::W, true);
    keys.SetKey(Key::W, true); // pressing twice is still one key
    EXPECT_TRUE(keys.IsKeyDown(Key::W));
    EXPECT_FALSE(keys.IsKeyDown(Key::A));
    EXPECT_TRUE(keys.AnyDown());

    keys.SetKey(Key::W, false);
    EXPECT_FALSE(keys.IsKeyDown(Key::W));
    EXPECT_FALSE(keys.AnyDown());
}

TEST(InjectedKeysTest, MouseButtonsAreSeparateFromKeysAndReleaseAllClearsBoth)
{
    InjectedKeys keys;
    keys.SetMouseButton(MouseButton::Right, true);
    keys.SetKey(Key::Space, true);
    EXPECT_TRUE(keys.IsMouseButtonDown(MouseButton::Right));
    EXPECT_FALSE(keys.IsMouseButtonDown(MouseButton::Left));
    EXPECT_TRUE(keys.IsKeyDown(Key::Space));

    keys.ReleaseAll();
    EXPECT_FALSE(keys.IsMouseButtonDown(MouseButton::Right));
    EXPECT_FALSE(keys.IsKeyDown(Key::Space));
    EXPECT_FALSE(keys.AnyDown());
}

TEST(KeySourceTest, NoSourceIsInstalledByDefault)
{
    EXPECT_EQ(KeySource::Get(), nullptr);
}

TEST(KeySourceTest, SetReturnsTheSourceItReplaced)
{
    InjectedKeys first;
    InjectedKeys second;
    EXPECT_EQ(KeySource::Set(&first), nullptr);
    EXPECT_EQ(KeySource::Get(), &first);
    EXPECT_EQ(KeySource::Set(&second), &first);
    EXPECT_EQ(KeySource::Set(nullptr), &second);
    EXPECT_EQ(KeySource::Get(), nullptr);
}

TEST(KeySourceTest, AKeyboardBindingReadsTheSourceWithoutAWindow)
{
    KeyboardButtonBinding binding(NoWindow(), Key::W);
    EXPECT_FALSE(binding.getValue().asBool()) << "no window and no source: nothing is pressed";

    InjectedKeys keys;
    const SourceScope scope(&keys);
    EXPECT_FALSE(binding.getValue().asBool());

    keys.SetKey(Key::W, true);
    EXPECT_TRUE(binding.getValue().asBool());

    keys.SetKey(Key::W, false);
    EXPECT_FALSE(binding.getValue().asBool());
}

TEST(KeySourceTest, ABindingReadsNothingOnceTheSourceIsGone)
{
    KeyboardButtonBinding binding(NoWindow(), Key::W);
    InjectedKeys keys;
    keys.SetKey(Key::W, true);
    {
        const SourceScope scope(&keys);
        EXPECT_TRUE(binding.getValue().asBool());
    }
    EXPECT_FALSE(binding.getValue().asBool());
}

TEST(KeySourceTest, ACompositeBindingAddsTheDirectionsThatAreDown)
{
    Vector2CompositeBinding binding(NoWindow(), Key::W, Key::S, Key::A, Key::D);
    InjectedKeys keys;
    const SourceScope scope(&keys);
    EXPECT_FLOAT_EQ(binding.getValue().asVector2().x, 0.0f);
    EXPECT_FLOAT_EQ(binding.getValue().asVector2().y, 0.0f);

    keys.SetKey(Key::D, true);
    keys.SetKey(Key::W, true);
    EXPECT_FLOAT_EQ(binding.getValue().asVector2().x, 1.0f);
    EXPECT_FLOAT_EQ(binding.getValue().asVector2().y, 1.0f);

    keys.SetKey(Key::A, true); // left and right cancel
    EXPECT_FLOAT_EQ(binding.getValue().asVector2().x, 0.0f);

    keys.SetKey(Key::W, false);
    keys.SetKey(Key::S, true);
    EXPECT_FLOAT_EQ(binding.getValue().asVector2().y, -1.0f);
}

TEST(KeySourceTest, AMouseButtonBindingReadsTheSource)
{
    MouseButtonBinding binding(NoWindow(), MouseButton::Left);
    InjectedKeys keys;
    const SourceScope scope(&keys);
    EXPECT_FALSE(binding.getValue().asBool());

    keys.SetMouseButton(MouseButton::Right, true);
    EXPECT_FALSE(binding.getValue().asBool()) << "another button";

    keys.SetMouseButton(MouseButton::Left, true);
    EXPECT_TRUE(binding.getValue().asBool());
}

TEST(KeySourceTest, InjectedKeysDriveAnInputAction)
{
    ActionMap map("Gameplay");
    map.MakeInputAction("Jump", [](InputAction *action)
    {
        action->AddBinding(std::make_unique<KeyboardButtonBinding>(NoWindow(), Key::Space));
    });
    InputAction &jump = map["Jump"];

    InjectedKeys keys;
    const SourceScope scope(&keys);

    map.Update();
    EXPECT_EQ(jump.GetPhase(), ActionPhase::Waiting);
    EXPECT_FALSE(jump.GetBoolValue());

    keys.SetKey(Key::Space, true);
    map.Update(); // Started
    map.Update(); // Performed
    EXPECT_EQ(jump.GetPhase(), ActionPhase::Performed);
    EXPECT_TRUE(jump.GetBoolValue());

    keys.SetKey(Key::Space, false);
    map.Update();
    EXPECT_FALSE(jump.GetBoolValue());
}
