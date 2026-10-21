#pragma once

#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include <math/MathRegistrar.hpp>
#include <math/Vector3.hpp>
#include "engine/network/NetworkSettings.hpp"
#include "engine/physics/GpuStatus.hpp"
#include "engine/physics/PhysicsBackendKind.hpp"
#include <renderer/common/SceneLighting.hpp>

namespace N2Engine
{
    /**
     * Applies a project file's settings object (IO::ProjectFile::settings) to the running engine, one block per
     * subsystem, each read by that subsystem's own deserializer:
     *
     *     layers    Layers::Deserialize ({"layers": [32 names], "collisionMatrix": [32 masks]})
     *     input     must be {"actionMaps": {...}}; the window's InputSystem::Deserialize reads it (without an input
     *               system the shape is still checked, then nothing is applied)
     *     physics   {"fixedTimestep": seconds > 0, "gravity": {"x", "y", "z"}}: Time::SetFixedTimestep, and the
     *               physics backend's gravity (skipped without a backend). "gpu": "off" | "auto" | "on" (default off,
     *               case-insensitive; anything else is refused with a warning and means off) is read by
     *               ReadPhysicsGpuMode before Application::Init, because the scene's GPU flag is fixed when it is
     *               created; applying it afterwards only validates it (a change needs a restart).
     *               "deterministic": true | false (default false; a non-boolean is refused and means false) asks the
     *               physics backend for its reproducible-replay configuration (PhysX: enhanced determinism and 0
     *               worker threads); read by ReadPhysicsDeterministic before Application::Init like gpu
     *               "backend": "physx" | "jolt" (default physx when unset, case-insensitive; anything else is refused
     *               with a warning and means physx) picks the project's 3D physics backend. Read by ReadPhysicsBackend
     *               before Application::Init creates the backend, so applying it afterwards only validates it (a change
     *               needs a restart)
     *     math      {"simdTier": "auto" | "scalar" | "sse2" | "sse4.1" | "avx" | "neon"} (default auto, case-insensitive;
     *               anything else is refused with a warning and means auto). Read by ReadMathSimdTier before
     *               Application::Init selects the math tier, so applying it afterwards only validates it (a change
     *               needs a restart)
     *     determinism {"luaSeed": integer >= 0}: unset by default. Set, Lua's string hash seed is a constant and
     *               math.random starts from that seed, so pairs() order over string keys and math.random are the same
     *               on every run (LuaRuntime::SetLuaSeed); anything but a non-negative integer is refused with a
     *               warning and means unset. Read by ReadDeterminismLuaSeed before Application::Init creates the Lua
     *               state, so applying it afterwards only validates it (a change needs a restart). SECURITY: a
     *               constant hash seed makes hash flooding of string-keyed tables possible, so leave it unset unless
     *               the script data is trusted; no host sets it implicitly
     *     network   the network model: {"preset": "none" | "authoritative" | "deterministic", "tickRate", "transport":
     *               {"udp", "webSocket", "port"}, "authoritative": {...}, "deterministic": {"mode": "lockstep" |
     *               "rollback", "inputDelay", "rollbackWindow", "hashInterval"}, "overrides": {}, "customised"}; see
     *               ReadNetworkSettings. Default preset none, which changes nothing. Read by Application::Init (which
     *               refuses to start a Deterministic project whose requirements are unmet), so applying it afterwards
     *               only validates it (a change needs a restart). A bad value is a warning and the default or the
     *               nearest allowed value is used; it never stops Init
     *     rendering {"colorSpace": "gamma" | "linear"}: Rendering::RenderSettings::SetColorSpace (case-insensitive;
     *               other keys in the block are left alone). A block without colorSpace, with it null, or (when
     *               `only` names rendering) removed, means the default, gamma. "forceShaderEncode": true makes
     *               OpenGL's lit shader encode its own output in linear lighting (an escape hatch for a driver that
     *               reports the wrong framebuffer encoding); absent, null or removed means false, a non-boolean is
     *               refused; a bad value is refused and changes nothing
     *
     * Other blocks (window, and keys this version doesn't know) are left to whoever reads them: an editor
     * host has no window to size. A missing or null block changes nothing (except rendering when `only` names it, above). Call it after Application::Init (which
     * resets the fixed timestep). Main thread.
     *
     * only, when given, limits it to those top-level blocks (the ones a settings patch touched).
     *
     * Returns one message per block that was rejected (its subsystem refused it, or it has the wrong shape); empty
     * when everything given was applied. A rejected block may have been partly applied (physics applies
     * fixedTimestep before it checks gravity): a caller that must undo takes a ProjectSettingsSnapshot first.
     */
    [[nodiscard]] std::vector<std::string> ApplyProjectSettings(const nlohmann::json &settings,
                                                                const std::optional<std::set<std::string>> &only = {});

    /**
     * physics.gpu from a project's settings object: Off when it is absent, null or not "off"/"auto"/"on"
     * (case-insensitive). Host code calls it before Application::Init (the physics scene is built there).
     * problems, when given, gets one message for an unknown value.
     */
    [[nodiscard]] Physics::GpuMode ReadPhysicsGpuMode(const nlohmann::json &settings,
                                                      std::vector<std::string> *problems = nullptr);

    /**
     * physics.deterministic from a project's settings object: false when it is absent, null or not a boolean.
     * Host code calls it before Application::Init (the physics scene is built there).
     * problems, when given, gets one message for a value that is not a boolean.
     */
    [[nodiscard]] bool ReadPhysicsDeterministic(const nlohmann::json &settings,
                                                std::vector<std::string> *problems = nullptr);

    /**
     * physics.backend from a project's settings object: empty when it is absent or null (the host's option applies,
     * and with AUTO PhysX when it is in the build, else Jolt), "physx" or "jolt" (case-insensitive) as written, and
     * PhysX for anything else, with one message
     * in `problems` (when given). Application::Init calls it before the physics backend is created and applies
     * Physics::ChooseBackend to the result.
     */
    [[nodiscard]] std::optional<Physics::PhysicsBackendKind> ReadPhysicsBackend(
        const nlohmann::json &settings, std::vector<std::string> *problems = nullptr);

    /**
     * math.simdTier from a project's settings object: Auto when it is absent, null or not a known tier name
     * (see Math::ParseSIMDTierRequest). Application::Init calls it before the math tier is chosen, so before any
     * scene loads. problems, when given, gets one message for an unknown value.
     */
    [[nodiscard]] Math::SIMDTierRequest ReadMathSimdTier(const nlohmann::json &settings,
                                                         std::vector<std::string> *problems = nullptr);

    /**
     * determinism.luaSeed from a project's settings object: empty (unset) when it is absent, null or not a
     * non-negative integer (a float such as 5.0, a string, a boolean and a negative number are refused). Application::Init
     * calls it before the Lua state is created. problems, when given, gets one message for a refused value.
     */
    [[nodiscard]] std::optional<std::uint64_t> ReadDeterminismLuaSeed(const nlohmann::json &settings,
                                                                      std::vector<std::string> *problems = nullptr);

    /**
     * The "network" block of a project's settings object, every value validated: a project without the block, or with
     * a block that is not an object, gets the defaults (preset none). A value of the wrong type, an unknown preset or
     * mode name, an integer setting that is not a whole number, or an override of a key the presets do not govern is
     * refused with one message in `problems` (when given) and replaced by its default (or ignored, for an override);
     * a number outside its allowed range (Network::NumericSettingSpecs) is clamped to it, also with one message. It
     * never throws and a bad value never stops Application::Init. Names are case-insensitive. Application::Init calls it
     * before the physics backend is created.
     */
    [[nodiscard]] Network::NetworkSettings ReadNetworkSettings(const nlohmann::json &settings,
                                                               std::vector<std::string> *problems = nullptr);

    /**
     * The live state ApplyProjectSettings changes, taken before applying so it can be put back exactly (a block the
     * previous settings didn't have would otherwise keep the new value): the layers (Layers::Serialize), the input
     * maps (when there is an input system), the unscaled fixed timestep and the backend's gravity (when there is a
     * backend), and the colour space.
     */
    struct ProjectSettingsSnapshot
    {
        nlohmann::json layers;
        std::optional<nlohmann::json> input;
        double fixedTimestep = 0.0;
        std::optional<Math::Vector3> gravity;
        Renderer::Common::ColorSpace colorSpace = Renderer::Common::ColorSpace::Gamma;
        bool forceShaderEncode = false;

        [[nodiscard]] static ProjectSettingsSnapshot Capture();
        /// Puts the captured state back
        void Restore() const;
    };
}
