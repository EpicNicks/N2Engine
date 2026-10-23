#include "engine/replay/ReplayLog.hpp"

#include <fstream>
#include <iterator>
#include <limits>

#include <nlohmann/json.hpp>

namespace N2Engine::Replay
{
    namespace
    {
        constexpr const char *kFormatName = "n2replay";

        bool ReadInt(const nlohmann::json &value, const long long lo, const long long hi, int &out)
        {
            if (!value.is_number_integer())
            {
                return false;
            }
            const long long v = value.get<long long>();
            if (v < lo || v > hi)
            {
                return false;
            }
            out = static_cast<int>(v);
            return true;
        }

        bool ReadU32(const nlohmann::json &value, std::uint32_t &out)
        {
            if (!value.is_number_integer())
            {
                return false;
            }
            const long long v = value.get<long long>();
            if (v < 0 || v > static_cast<long long>(std::numeric_limits<std::uint32_t>::max()))
            {
                return false;
            }
            out = static_cast<std::uint32_t>(v);
            return true;
        }
    }

    std::string ValidateReplayLog(const ReplayLog &log)
    {
        if (log.version != kReplayFormatVersion)
        {
            return "unknown version " + std::to_string(log.version);
        }
        if (log.scene.empty())
        {
            return "no scene name";
        }
        if (log.ticks < 0 || log.ticks > kMaxTicks)
        {
            return "ticks out of range";
        }
        if (log.hashInterval < 1 || log.hashInterval > kMaxTicks)
        {
            return "hashInterval out of range";
        }
        for (std::size_t i = 0; i < log.inputs.size(); ++i)
        {
            const ReplayInput &row = log.inputs[i];
            const std::string where = "input row " + std::to_string(i) + ": ";
            if (row.tick < 0 || row.tick >= log.ticks)
            {
                return where + "tick outside [0, ticks)";
            }
            if (row.slot < 0 || row.slot >= kMaxSlots)
            {
                return where + "slot outside [0, " + std::to_string(kMaxSlots) + ")";
            }
            if (row.moveX < -kMaxMove || row.moveX > kMaxMove || row.moveY < -kMaxMove || row.moveY > kMaxMove)
            {
                return where + "move outside [-127, 127]";
            }
            if (i > 0)
            {
                const ReplayInput &previous = log.inputs[i - 1];
                if (row.tick < previous.tick || (row.tick == previous.tick && row.slot <= previous.slot))
                {
                    return where + "rows are not strictly increasing by (tick, slot)";
                }
            }
        }
        return {};
    }

    std::optional<ReplayLog> ParseReplayLog(const std::string_view text, std::string &error)
    {
        const nlohmann::json root = nlohmann::json::parse(text.begin(), text.end(), nullptr, false);
        if (root.is_discarded() || !root.is_object())
        {
            error = "not a JSON object";
            return std::nullopt;
        }
        if (!root.contains("format") || !root["format"].is_string() || root["format"].get<std::string>() != kFormatName)
        {
            error = std::string("\"format\" is not \"") + kFormatName + "\"";
            return std::nullopt;
        }
        ReplayLog log;
        if (!root.contains("version") || !ReadInt(root["version"], 0, std::numeric_limits<int>::max(), log.version))
        {
            error = "no integer \"version\"";
            return std::nullopt;
        }
        if (log.version != kReplayFormatVersion)
        {
            error = "unknown version " + std::to_string(log.version) + " (this reader knows " +
                    std::to_string(kReplayFormatVersion) + ")";
            return std::nullopt;
        }
        if (!root.contains("scene") || !root["scene"].is_string())
        {
            error = "no string \"scene\"";
            return std::nullopt;
        }
        log.scene = root["scene"].get<std::string>();
        if (!root.contains("seed") || !ReadU32(root["seed"], log.seed))
        {
            error = "no u32 \"seed\"";
            return std::nullopt;
        }
        if (!root.contains("ticks") || !ReadInt(root["ticks"], 0, kMaxTicks, log.ticks))
        {
            error = "no integer \"ticks\" in [0, 1000000]";
            return std::nullopt;
        }
        if (!root.contains("hashInterval") || !ReadInt(root["hashInterval"], 1, kMaxTicks, log.hashInterval))
        {
            error = "no integer \"hashInterval\" in [1, 1000000]";
            return std::nullopt;
        }
        if (!root.contains("inputs") || !root["inputs"].is_array())
        {
            error = "no \"inputs\" array";
            return std::nullopt;
        }
        for (const nlohmann::json &row : root["inputs"])
        {
            ReplayInput input;
            if (!row.is_array() || row.size() != 5 || !ReadInt(row[0], 0, kMaxTicks, input.tick) ||
                !ReadInt(row[1], 0, kMaxSlots, input.slot) || !ReadInt(row[2], -kMaxMove, kMaxMove, input.moveX) ||
                !ReadInt(row[3], -kMaxMove, kMaxMove, input.moveY) || !ReadU32(row[4], input.buttons))
            {
                error = "input row " + std::to_string(log.inputs.size()) + " is not [tick, slot, moveX, moveY, buttons]";
                return std::nullopt;
            }
            log.inputs.push_back(input);
        }
        error = ValidateReplayLog(log);
        if (!error.empty())
        {
            return std::nullopt;
        }
        return log;
    }

    std::optional<ReplayLog> LoadReplayLog(const std::string &path, std::string &error)
    {
        std::ifstream in(path, std::ios::binary);
        if (!in)
        {
            error = "cannot read " + path;
            return std::nullopt;
        }
        const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        return ParseReplayLog(text, error);
    }

    std::string SerializeReplayLog(const ReplayLog &log)
    {
        std::string out = "{\n";
        out += std::string(" \"format\": \"") + kFormatName + "\",\n";
        out += " \"version\": " + std::to_string(log.version) + ",\n";
        out += " \"scene\": " + nlohmann::json(log.scene).dump() + ",\n";
        out += " \"seed\": " + std::to_string(log.seed) + ",\n";
        out += " \"ticks\": " + std::to_string(log.ticks) + ",\n";
        out += " \"hashInterval\": " + std::to_string(log.hashInterval) + ",\n";
        out += " \"inputs\": [";
        for (std::size_t i = 0; i < log.inputs.size(); ++i)
        {
            const ReplayInput &row = log.inputs[i];
            out += i == 0 ? "\n  [" : ",\n  [";
            out += std::to_string(row.tick) + ", " + std::to_string(row.slot) + ", " + std::to_string(row.moveX) + ", " +
                   std::to_string(row.moveY) + ", " + std::to_string(row.buttons) + "]";
        }
        out += log.inputs.empty() ? "]\n}\n" : "\n ]\n}\n";
        return out;
    }
}
