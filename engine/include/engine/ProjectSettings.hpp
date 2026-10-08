#pragma once

#include <string>
#include <vector>

#include <nlohmann/json_fwd.hpp>

namespace N2Engine
{
    /**
     * Applies a project file's settings object (IO::ProjectFile::settings) to the running engine, one block per
     * subsystem, each read by that subsystem's own deserializer:
     *
     *     layers    Layers::Deserialize ({"layers": [32 names], "collisionMatrix": [32 masks]})
     *     input     the window's InputSystem::Deserialize (skipped without an input system)
     *     physics   {"fixedTimestep": seconds > 0, "gravity": {"x", "y", "z"}}: Time::SetFixedTimestep, and the
     *               physics backend's gravity (skipped without a backend)
     *
     * Other blocks (window, rendering, and keys this version doesn't know) are left to whoever reads them: an editor
     * host has no window to size. A missing or null block changes nothing. Call it after Application::Init (which
     * resets the fixed timestep). Main thread.
     *
     * Returns one message per block that was rejected (its subsystem refused it, or it has the wrong shape); empty
     * when everything given was applied. A rejected block changes nothing where its subsystem promises so (Layers);
     * the other blocks are still applied.
     */
    [[nodiscard]] std::vector<std::string> ApplyProjectSettings(const nlohmann::json &settings);
}
