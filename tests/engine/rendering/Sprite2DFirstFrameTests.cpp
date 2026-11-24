// A Sprite2D created during a frame draws in that frame's render (issue #445; docs/sprite2d.html#first-frame): the scene
// collects its renderables by walking the hierarchy when it draws, not from the attach queue, and the sprite builds its mesh
// and material and uploads them in its own draw call. Its OnAttach (and that of any component added with it) comes at the
// start of the next frame. Frames run through Application::Tick on a headless software window, as a game's do.

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <math/Quaternion.hpp>
#include <math/Vector2.hpp>
#include <math/Vector3.hpp>

#include "engine/Application.hpp"
#include "engine/Camera.hpp"
#include "engine/GameObjectScene.hpp"
#include "engine/Positionable.hpp"
#include "engine/Time.hpp"
#include "engine/Window.hpp"
#include "engine/common/Color.hpp"
#include "engine/config/ApplicationOptions.hpp"
#include "engine/rendering/Sprite2D.hpp"
#include "engine/sceneManagement/Scene.hpp"
#include "engine/sceneManagement/SceneManager.hpp"
#ifdef N2ENGINE_SCRIPTING_ENABLED
#include <filesystem>
#include <fstream>

#include <math/UUID.hpp>

#include "engine/io/ResourceLoader.hpp"
#include "engine/io/ResourcePath.hpp"
#include "engine/io/ResourceUUID.hpp"
#include "engine/scripting/LuaComponent.hpp"
#include "engine/scripting/LuaRuntime.hpp"
#endif

using namespace N2Engine;

namespace
{
    /// Counts OnAttach calls: added beside a spawned sprite, it shows the sprite drew before its components attached
    class AttachProbe final : public Component
    {
    public:
        explicit AttachProbe(GameObject &gameObject) : Component(gameObject) {}
        [[nodiscard]] std::string GetTypeName() const override { return "Sprite2DFirstFrameTest_AttachProbe"; }
        void OnAttach() override { ++attaches; }
        static inline int attaches = 0;
    };

    /// Makes one large red sprite in front of the camera in its first OnUpdate
    class Spawner final : public Component
    {
    public:
        explicit Spawner(GameObject &gameObject) : Component(gameObject) {}
        [[nodiscard]] std::string GetTypeName() const override { return "Sprite2DFirstFrameTest_Spawner"; }

        void OnUpdate() override
        {
            if (spawned)
            {
                return;
            }
            spawned = true;
            const auto go = GameObject::Create("SpawnedSprite");
            go->CreatePositionable();
            // In front of both cameras Application::Render can use (the fallback at the origin, the main one at z = 10;
            // both look down -Z), and large enough to cover the whole frame of either
            go->GetPositionable()->SetPosition(Math::Vector3(0.0f, 0.0f, -5.0f));
            auto *sprite = go->AddComponent<Rendering::Sprite2D>();
            sprite->SetSize(Math::Vector2(200.0f, 200.0f));
            sprite->SetColor(Common::Color::Red);
            go->AddComponent<AttachProbe>();
            SceneManager::GetCurSceneRef().AddRootGameObject(go);
        }

        bool spawned = false;
    };

    class Sprite2DFirstFrameTest : public ::testing::Test
    {
    protected:
        static constexpr int Width = 16;
        static constexpr int Height = 16;
        static inline int sceneCounter = 0;
        Common::Color _savedClear = Common::Color::Black;
        std::optional<Camera> _savedMainCamera;

        void SetUp() override
        {
            Config::ApplicationOptions options;
            options.renderBackend = Config::ApplicationOptions::RenderBackend::SOFTWARE;
            options.isHeadless = true;
            Window &window = Application::GetInstance().GetWindow();
            ASSERT_TRUE(window.InitWindow(options)) << window.GetInitError();
            ASSERT_TRUE(window.SetRenderSize(Width, Height));
            _savedClear = window.clearColor;
            window.clearColor = Common::Color::Black;
            Application::GetInstance().Set3DPhysicsBackend(nullptr);
            // The main camera exists once some test ran Application::Init, and others move it: a known view for this one
            if (Camera *main = Application::GetInstance().GetMainCamera())
            {
                _savedMainCamera = *main;
                main->SetPerspective(45.0f, 1.0f, 0.1f, 100.0f);
                main->SetPosition(Math::Vector3(0.0f, 0.0f, 10.0f));
                main->SetRotation(Math::Quaternion::Identity);
            }

            SceneManager::AddScene(Scene::Create("Sprite2DFirstFrameTest_" + std::to_string(++sceneCounter)), true);
            SceneManager::ProcessAnyPendingSceneChange();
            ASSERT_NE(SceneManager::GetCurScene(), nullptr);
            AttachProbe::attaches = 0;
        }

        void TearDown() override
        {
            // Other tests rely on the engine clock standing still: a frame of no time leaves the deltas at zero again
            TickOptions rest;
            rest.render = false;
            rest.pollEvents = false;
            rest.deltaSeconds = 0.0;
            Application::GetInstance().Tick(rest);

            SceneManager::AddScene(Scene::Create("Sprite2DFirstFrameTest_Empty_" + std::to_string(++sceneCounter)), true);
            SceneManager::ProcessAnyPendingSceneChange();
            Window &window = Application::GetInstance().GetWindow();
            window.clearColor = _savedClear;
            window.Shutdown();
            if (Camera *main = Application::GetInstance().GetMainCamera(); main && _savedMainCamera)
            {
                *main = *_savedMainCamera;
            }
        }

        /// One whole frame (attach queue, Update, coroutines, LateUpdate, draw), then the centre pixel it drew
        static std::vector<int> FrameCentre()
        {
            TickOptions options;
            options.pollEvents = false;
            options.render = true;
            options.deltaSeconds = Time::GetFixedTimestep();
            Application::GetInstance().Tick(options);

            auto *renderer = Application::GetInstance().GetWindow().GetRenderer();
            std::vector<std::uint8_t> pixels(static_cast<std::size_t>(Width) * Height * 4);
            renderer->ReadFramebuffer(pixels.data(), Width, Height);
            const std::size_t i = (static_cast<std::size_t>(Height / 2) * Width + Width / 2) * 4;
            return {pixels[i], pixels[i + 1], pixels[i + 2]};
        }

        static bool IsRed(const std::vector<int> &c) { return c[0] > 200 && c[1] < 60 && c[2] < 60; }
        static bool IsGreen(const std::vector<int> &c) { return c[1] > 200 && c[0] < 60 && c[2] < 60; }
        static bool IsBlack(const std::vector<int> &c) { return c[0] < 30 && c[1] < 30 && c[2] < 30; }
    };
}

TEST_F(Sprite2DFirstFrameTest, ASpriteCreatedInOnUpdateDrawsInThatFramesRender)
{
    const auto spawner = GameObject::Create("Spawner");
    SceneManager::GetCurSceneRef().AddRootGameObject(spawner);
    spawner->AddComponent<Spawner>();

    const std::vector<int> first = FrameCentre(); // the spawner attaches, then its OnUpdate makes the sprite
    ASSERT_TRUE(spawner->GetComponent<Spawner>()->spawned);
    EXPECT_TRUE(IsRed(first)) << first[0] << "," << first[1] << "," << first[2];
    EXPECT_EQ(AttachProbe::attaches, 0) << "drawn before the new object's components attached";

    const std::vector<int> second = FrameCentre();
    EXPECT_TRUE(IsRed(second));
    EXPECT_EQ(AttachProbe::attaches, 1) << "its OnAttach runs at the start of the next frame";
}

TEST_F(Sprite2DFirstFrameTest, NothingDrawsBeforeTheSpriteExists)
{
    const std::vector<int> empty = FrameCentre();
    EXPECT_TRUE(IsBlack(empty)) << empty[0] << "," << empty[1] << "," << empty[2];
}

#ifdef N2ENGINE_SCRIPTING_ENABLED
TEST_F(Sprite2DFirstFrameTest, ASpriteCreatedFromLuaInOnUpdateDrawsInThatFramesRender)
{
    namespace fs = std::filesystem;
    ASSERT_TRUE(Scripting::LuaRuntime::Instance().Initialize());
    const fs::path root = fs::temp_directory_path() / "n2engine_sprite2d_first_frame_tests";
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root / "assets" / "first");
    std::ofstream(root / "assets" / "first" / "Spawner.lua") << R"(
        local Spawner = {}
        Spawner.__index = Spawner
        function Spawner:OnUpdate()
            if self.spawned then return end
            self.spawned = true
            local go = GameObject.Create("LuaSprite")
            go:CreatePositionable()
            go:GetPositionable():SetPosition(Vector3(0, 0, -5))
            local sprite = go:AddComponent("Sprite2D")
            sprite:SetSize(Vector2(200, 200))
            sprite:SetColor(Color.Green)
            SceneManager.GetCurrentScene():AddRootGameObject(go)
            first_frame_spawned = true
        end
        return Spawner
    )";
    IO::ResourceUUID::Initialize(Math::UUID::Random());
    IO::ResourceLoader::Instance().Initialize(root);

    auto &lua = Scripting::LuaRuntime::Instance().GetState();
    lua["first_frame_spawned"] = false;
    const auto spawner = GameObject::Create("LuaSpawner");
    SceneManager::GetCurSceneRef().AddRootGameObject(spawner);
    auto *component = spawner->AddComponent<Scripting::LuaComponent>();
    component->SetScript(IO::ResourcePath("res://first/Spawner.lua"));
    ASSERT_FALSE(component->HasMissingScript());

    const std::vector<int> first = FrameCentre();
    EXPECT_TRUE(lua["first_frame_spawned"].get<bool>());
    EXPECT_FALSE(component->IsFaulted());
    EXPECT_TRUE(IsGreen(first)) << first[0] << "," << first[1] << "," << first[2];

    lua["first_frame_spawned"] = sol::lua_nil;
    fs::remove_all(root, ec);
}
#endif
