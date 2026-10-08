#pragma once

#include <optional>
#include <set>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include <math/Vector3.hpp>
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
     *               physics backend's gravity (skipped without a backend)
     *     rendering {"colorSpace": "gamma" | "linear"}: Rendering::RenderSettings::SetColorSpace (case-insensitive;
     *               other keys in the block are left alone; a missing colorSpace changes nothing)
     *
     * Other blocks (window, and keys this version doesn't know) are left to whoever reads them: an editor
     * host has no window to size. A missing or null block changes nothing. Call it after Application::Init (which
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

        [[nodiscard]] static ProjectSettingsSnapshot Capture();
        /// Puts the captured state back
        void Restore() const;
    };
}
