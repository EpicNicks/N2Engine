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
        PhysX, // the default when it is in the build (ChooseBackend)
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

    /// Which backends this build contains (IsBackendCompiledIn for each); a parameter of ChooseBackend so every
    /// combination of build flags can be tested in any build
    struct BackendAvailability
    {
        bool physx = false;
        bool jolt = false;

        [[nodiscard]] constexpr bool Has(const PhysicsBackendKind kind) const
        {
            return kind == PhysicsBackendKind::PhysX ? physx : jolt;
        }
        [[nodiscard]] constexpr bool Any() const { return physx || jolt; }
    };

    /// The backends of this build
    [[nodiscard]] constexpr BackendAvailability CompiledInBackends()
    {
        return {.physx = IsBackendCompiledIn(PhysicsBackendKind::PhysX),
                .jolt = IsBackendCompiledIn(PhysicsBackendKind::Jolt)};
    }

    /// Where a backend request comes from, so a message names the right source
    enum class BackendRequestSource
    {
        Auto,           // nobody asked: no physics.backend and the host's ApplicationOptions::physicsBackend is AUTO
        HostOption,     // the host's ApplicationOptions::physicsBackend (PHYSX or JOLT); no physics.backend is set
        ProjectSetting  // the project's physics.backend (it wins over the host option)
    };

    /// How a message names a request: "physics.backend is jolt" or "the host option (ApplicationOptions::physicsBackend)
    /// is jolt and the project sets no physics.backend". `name` is ToString of the backend.
    [[nodiscard]] inline std::string DescribeBackendRequest(const std::string_view name,
                                                            const BackendRequestSource source)
    {
        if (source == BackendRequestSource::HostOption)
        {
            return std::format("the host option (ApplicationOptions::physicsBackend) is {} and the project sets no "
                               "physics.backend",
                               name);
        }
        return std::format("physics.backend is {}", name);
    }

    /// What Application::Init does with the backend a project (or its host) asked for
    struct BackendChoice
    {
        /// What was asked for; nullopt when nobody asked (source Auto)
        std::optional<PhysicsBackendKind> requested;
        BackendRequestSource source = BackendRequestSource::Auto;
        /// The backend Init creates; nullopt: none (the build has no usable backend, or the start is refused)
        std::optional<PhysicsBackendKind> selected;
        /// The requested backend is not in this build and the preset is not Deterministic: the other backend, which
        /// is in the build, is used instead (warning)
        bool fellBack = false;
        /// The requested backend is not in this build and nothing is put in its place (error): the Deterministic
        /// preset never moves to another backend (Init then refuses the project), and in any other preset there is no
        /// other backend in the build (Init runs without physics)
        bool refused = false;
        /// Empty when nothing was unusual; otherwise the one line Init logs (an error when refused, a warning when it
        /// fell back, information when nobody asked and the build has no backend)
        std::string message;
    };

    /**
     * The selection rule, a pure function (Application::Init applies it before any backend exists):
     *   nobody asked (Auto)                        -> PhysX when it is in the build, else Jolt when it is, else none
     *                                                 (with an information message)
     *   the requested backend is in the build      -> it is selected, no message
     *   not in the build, Deterministic preset     -> refused (the Network row names it; nothing starts)
     *   not in the build, another one is           -> falls back to that one with a warning
     *   not in the build, no backend in the build  -> refused: no physics, with an error
     * A fallback only ever names a backend that is in the build. `available` is CompiledInBackends() in Init, a
     * parameter so every combination of build flags can be tested.
     */
    [[nodiscard]] inline BackendChoice ChooseBackend(const std::optional<PhysicsBackendKind> requested,
                                                     const BackendRequestSource source,
                                                     const bool deterministicPreset,
                                                     const BackendAvailability available)
    {
        BackendChoice choice;
        choice.requested = requested;
        choice.source = requested ? source : BackendRequestSource::Auto;
        if (!requested)
        {
            if (available.physx)
            {
                choice.selected = PhysicsBackendKind::PhysX;
            }
            else if (available.jolt)
            {
                choice.selected = PhysicsBackendKind::Jolt;
            }
            else
            {
                choice.message = "this build has no physics backend (N2ENGINE_USE_PHYSX and N2ENGINE_USE_JOLT are "
                                 "off): physics is disabled";
            }
            return choice;
        }
        const PhysicsBackendKind kind = *requested;
        if (available.Has(kind))
        {
            choice.selected = kind;
            return choice;
        }
        const std::string asked = DescribeBackendRequest(ToString(kind), choice.source);
        if (deterministicPreset)
        {
            choice.refused = true;
            choice.message = std::format("{} but this build has no {} backend: the Deterministic preset never falls "
                                         "back to another backend, so the project is refused",
                                         asked, DisplayName(kind));
            return choice;
        }
        const PhysicsBackendKind other =
            kind == PhysicsBackendKind::PhysX ? PhysicsBackendKind::Jolt : PhysicsBackendKind::PhysX;
        if (available.Has(other))
        {
            choice.fellBack = true;
            choice.selected = other;
            choice.message = std::format("{} but this build has no {} backend; using {} instead", asked,
                                         DisplayName(kind), ToString(other));
            return choice;
        }
        choice.refused = true;
        choice.message = std::format("{} but this build has no physics backend at all (N2ENGINE_USE_PHYSX and "
                                     "N2ENGINE_USE_JOLT are off): physics is disabled",
                                     asked);
        return choice;
    }
}
