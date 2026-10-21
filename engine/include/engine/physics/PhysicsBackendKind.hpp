#pragma once

#include <cctype>
#include <format>
#include <optional>
#include <string>
#include <string_view>

namespace N2Engine::Physics
{
    /// Which 3D physics backend a project runs on (the project setting physics.backend). One backend per project: it is
    /// chosen once in Application::Init and lives across scene switches.
    enum class PhysicsBackendKind
    {
        PhysX, // the default
        Jolt   // the deterministic backend (JoltBackend, in a build with N2ENGINE_USE_JOLT)
    };

    /// The value of the physics.backend setting: "physx" or "jolt" (also IPhysicsBackend::BackendId's prefix)
    [[nodiscard]] constexpr std::string_view ToString(const PhysicsBackendKind kind)
    {
        switch (kind)
        {
        case PhysicsBackendKind::PhysX:
            return "physx";
        case PhysicsBackendKind::Jolt:
            return "jolt";
        }
        return "physx";
    }

    /// The name the health row shows: "PhysX" or "Jolt"
    [[nodiscard]] constexpr std::string_view DisplayName(const PhysicsBackendKind kind)
    {
        switch (kind)
        {
        case PhysicsBackendKind::PhysX:
            return "PhysX";
        case PhysicsBackendKind::Jolt:
            return "Jolt";
        }
        return "PhysX";
    }

    /// "physx" or "jolt" (case-insensitive); nullopt for anything else
    [[nodiscard]] inline std::optional<PhysicsBackendKind> ParsePhysicsBackendKind(const std::string_view text)
    {
        std::string lower;
        lower.reserve(text.size());
        for (const char c : text)
        {
            lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        }
        if (lower == "physx")
        {
            return PhysicsBackendKind::PhysX;
        }
        if (lower == "jolt")
        {
            return PhysicsBackendKind::Jolt;
        }
        return std::nullopt;
    }

    /// Whether this build contains the backend: PhysX with N2ENGINE_USE_PHYSX, Jolt with N2ENGINE_USE_JOLT (a project
    /// that asks for one the build lacks gets the refusal or fallback of ChooseBackend)
    [[nodiscard]] constexpr bool IsBackendCompiledIn(const PhysicsBackendKind kind)
    {
        switch (kind)
        {
        case PhysicsBackendKind::PhysX:
#ifdef N2ENGINE_PHYSX_ENABLED
            return true;
#else
            return false;
#endif
        case PhysicsBackendKind::Jolt:
#ifdef N2ENGINE_JOLT_ENABLED
            return true;
#else
            return false;
#endif
        }
        return false;
    }

    /// What Application::Init does with the backend a project asked for
    struct BackendChoice
    {
        /// What the project (or, without a setting, the host) asked for
        PhysicsBackendKind requested = PhysicsBackendKind::PhysX;
        /// The backend Init creates; equals requested unless fellBack
        PhysicsBackendKind selected = PhysicsBackendKind::PhysX;
        /// The requested backend is not in this build and the project is not Deterministic: PhysX is used instead
        bool fellBack = false;
        /// The requested backend is not in this build and the project is Deterministic: the start is refused (a
        /// Deterministic project is never silently moved to a backend that cannot keep its promise)
        bool refused = false;
        /// Empty when nothing was unusual; otherwise the one line Init logs (an error when refused, else a warning)
        std::string message;
    };

    /**
     * The selection rule, a pure function (Application::Init applies it before any backend exists):
     *   requested backend in the build            -> it is selected, no message
     *   not in the build, Deterministic preset    -> refused (the Network row names it; nothing starts)
     *   not in the build, any other preset        -> falls back to PhysX with a warning (a PhysX request that PhysX
     *                                                itself cannot serve is left to Init's "built without PhysX" path)
     * `requestedCompiledIn` is IsBackendCompiledIn(requested), a parameter so every combination can be tested.
     */
    [[nodiscard]] inline BackendChoice ChooseBackend(const PhysicsBackendKind requested, const bool deterministicPreset,
                                                     const bool requestedCompiledIn)
    {
        BackendChoice choice;
        choice.requested = requested;
        choice.selected = requested;
        if (requestedCompiledIn || requested == PhysicsBackendKind::PhysX)
        {
            return choice;
        }
        if (deterministicPreset)
        {
            choice.refused = true;
            choice.message = std::format(
                "physics.backend is {} but this build has no {} backend: the Deterministic preset needs it, so the "
                "project is refused (it never falls back to another backend)",
                ToString(requested), DisplayName(requested));
            return choice;
        }
        choice.fellBack = true;
        choice.selected = PhysicsBackendKind::PhysX;
        choice.message = std::format("physics.backend is {} but this build has no {} backend; using {} instead",
                                     ToString(requested), DisplayName(requested),
                                     ToString(PhysicsBackendKind::PhysX));
        return choice;
    }
}
