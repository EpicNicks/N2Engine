#pragma once

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

namespace N2Engine
{
    enum class SubsystemState
    {
        NotStarted, // Init hasn't reached this subsystem
        Running,
        Disabled,   // Deliberately not started (e.g. audio in headless mode)
        Failed,
    };

    struct SubsystemStatus
    {
        std::string name;
        SubsystemState state = SubsystemState::NotStarted;
        std::string detail;
    };

    struct EngineHealth
    {
        std::vector<SubsystemStatus> subsystems;

        /// True when no subsystem has failed. Disabled and not-started subsystems don't count as failures.
        [[nodiscard]] bool IsHealthy() const
        {
            return std::ranges::none_of(subsystems, [](const SubsystemStatus &s) {
                return s.state == SubsystemState::Failed;
            });
        }

        [[nodiscard]] const SubsystemStatus* Find(std::string_view name) const
        {
            const auto it = std::ranges::find(subsystems, name, &SubsystemStatus::name);
            return it != subsystems.end() ? &*it : nullptr;
        }
    };

    [[nodiscard]] constexpr std::string_view ToString(SubsystemState state)
    {
        switch (state)
        {
        case SubsystemState::NotStarted:
            return "NotStarted";
        case SubsystemState::Running:
            return "Running";
        case SubsystemState::Disabled:
            return "Disabled";
        case SubsystemState::Failed:
            return "Failed";
        }
        return "Unknown";
    }
}
