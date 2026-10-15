#pragma once

#include <optional>
#include <set>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include <math/MathRegistrar.hpp>
#include <math/Vector3.hpp>
#include "engine/physics/GpuStatus.hpp"
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
     *               created; applying it afterwards only validates it (a change needs a restart)
     *     math      {"simdTier": "auto" | "scalar" | "sse2" | "sse4.1" | "avx" | "neon"} (default auto, case-insensitive;
     *               anything else is refused with a warning and means auto). Read by ReadMathSimdTier before
     *               Application::Init selects the math tier, so applying it afterwards only validates it (a change
     *               needs a restart)
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
     * math.simdTier from a project's settings object: Auto when it is absent, null or not a known tier name
     * (see Math::ParseSIMDTierRequest). Application::Init calls it before the math tier is chosen, so before any
     * scene loads. problems, when given, gets one message for an unknown value.
     */
    [[nodiscard]] Math::SIMDTierRequest ReadMathSimdTier(const nlohmann::json &settings,
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
