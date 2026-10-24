#include "engine/replay/ReplayLog.hpp"

#include <cmath>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <limits>

#include <nlohmann/json.hpp>

namespace N2Engine::Replay
{
    namespace
    {
        constexpr const char *kFormatName = "n2replay";
        /// No file has more rows than this: (tick, slot) strictly increases with tick < kMaxTicks and slot < kMaxSlots
        constexpr std::size_t kMaxRows = static_cast<std::size_t>(kMaxTicks) * static_cast<std::size_t>(kMaxSlots);

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

        std::string HashText(const std::uint64_t hash)
        {
            char buffer[24];
            std::snprintf(buffer, sizeof(buffer), "0x%016llx", static_cast<unsigned long long>(hash));
            return buffer;
        }

        std::string HexText(const std::vector<std::uint8_t> &bytes)
        {
            static constexpr char digits[] = "0123456789abcdef";
            std::string text;
            text.reserve(bytes.size() * 2);
            for (const std::uint8_t b : bytes)
            {
                text.push_back(digits[b >> 4]);
                text.push_back(digits[b & 0xF]);
            }
            return text;
        }

        int HexValue(const char c)
        {
            if (c >= '0' && c <= '9')
            {
                return c - '0';
            }
            if (c >= 'a' && c <= 'f')
            {
                return c - 'a' + 10;
            }
            if (c >= 'A' && c <= 'F')
            {
                return c - 'A' + 10;
            }
            return -1;
        }

        /// Exactly `bytes` bytes of hex, else false. The length is checked before anything is allocated.
        bool ParseHex(const std::string &text, const std::size_t bytes, std::vector<std::uint8_t> &out)
        {
            if (text.size() != bytes * 2)
            {
                return false;
            }
            out.assign(bytes, 0);
            for (std::size_t i = 0; i < bytes; ++i)
            {
                const int hi = HexValue(text[i * 2]);
                const int lo = HexValue(text[i * 2 + 1]);
                if (hi < 0 || lo < 0)
                {
                    return false;
                }
                out[i] = static_cast<std::uint8_t>(hi * 16 + lo);
            }
            return true;
        }

        int ToMove(const float value, const float range)
        {
            const double scaled = std::floor(static_cast<double>(value) / static_cast<double>(range) * 127.0 + 0.5);
            if (scaled < -kMaxMove)
            {
                return -kMaxMove;
            }
            if (scaled > kMaxMove)
            {
                return kMaxMove;
            }
            return static_cast<int>(scaled);
        }
    }

    bool DeriveSceneInput(const Network::InputLayout &layout, const std::span<const std::uint8_t> payload, int &moveX,
                          int &moveY, std::uint32_t &buttons)
    {
        const std::optional<std::vector<float>> values = layout.Unpack(payload);
        if (!values)
        {
            return false;
        }
        moveX = 0;
        moveY = 0;
        buttons = 0;

        const std::optional<std::size_t> move = layout.FindControl("move");
        const std::optional<std::size_t> axisX = layout.FindControl("moveX");
        const std::optional<std::size_t> axisY = layout.FindControl("moveY");
        if (move && (layout.Control(*move).kind == Network::InputControlKind::Vector2 ||
                     layout.Control(*move).kind == Network::InputControlKind::Vector3))
        {
            const Network::InputControlSpec &control = layout.Control(*move);
            moveX = ToMove((*values)[layout.ValueOffset(*move)], control.range);
            moveY = ToMove((*values)[layout.ValueOffset(*move) + 1], control.range);
        }
        else
        {
            if (axisX && layout.Control(*axisX).kind == Network::InputControlKind::Axis)
            {
                moveX = ToMove((*values)[layout.ValueOffset(*axisX)], layout.Control(*axisX).range);
            }
            if (axisY && layout.Control(*axisY).kind == Network::InputControlKind::Axis)
            {
                moveY = ToMove((*values)[layout.ValueOffset(*axisY)], layout.Control(*axisY).range);
            }
        }

        unsigned bit = 0;
        for (std::size_t i = 0; i < layout.ControlCount() && bit < 32; ++i)
        {
            if (layout.Control(i).kind != Network::InputControlKind::Button)
            {
                continue;
            }
            if ((*values)[layout.ValueOffset(i)] >= 0.5f)
            {
                buttons |= 1u << bit;
            }
            ++bit;
        }
        return true;
    }

    std::string ValidateReplayLog(const ReplayLog &log)
    {
        if (log.version != kReplayFormatVersion && log.version != kReplayFormatVersionLayout)
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
        const bool v2 = log.version == kReplayFormatVersionLayout;
        Network::InputLayout layout;
        if (v2)
        {
            if (log.layout.size() > Network::kMaxInputControls)
            {
                return "layout has too many controls";
            }
            auto created = Network::InputLayout::Create(log.layout);
            if (!created)
            {
                return "layout: " + created.error();
            }
            layout = std::move(*created);
        }
        else if (!log.layout.empty())
        {
            return "a version 1 log has no layout";
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
            if (v2)
            {
                int moveX = 0;
                int moveY = 0;
                std::uint32_t buttons = 0;
                if (row.payload.size() != layout.PayloadBytes() || !DeriveSceneInput(layout, row.payload, moveX, moveY, buttons))
                {
                    return where + "payload is not one the layout produces";
                }
                if (moveX != row.moveX || moveY != row.moveY || buttons != row.buttons)
                {
                    return where + "scene input does not match the payload";
                }
            }
            else if (!row.payload.empty())
            {
                return where + "a version 1 row has no payload";
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

    namespace
    {
        /// The layout header of a version 2 file; false with `error` set
        bool ParseLayout(const nlohmann::json &root, ReplayLog &log, Network::InputLayout &layout, std::string &error)
        {
            if (!root.contains("layout") || !root["layout"].is_array())
            {
                error = "no \"layout\" array";
                return false;
            }
            const nlohmann::json &array = root["layout"];
            // The size is checked before any control is built
            if (array.size() > Network::kMaxInputControls)
            {
                error = "\"layout\" has more than " + std::to_string(Network::kMaxInputControls) + " controls";
                return false;
            }
            std::vector<Network::InputControlSpec> specs;
            specs.reserve(array.size());
            for (const nlohmann::json &entry : array)
            {
                Network::InputControlSpec spec;
                int bits = 0;
                if (!entry.is_object() || !entry.contains("name") || !entry["name"].is_string() || !entry.contains("kind") ||
                    !entry["kind"].is_string() || !entry.contains("bits") || !ReadInt(entry["bits"], 0, 255, bits) ||
                    !entry.contains("range") || !entry["range"].is_number() || !entry.contains("signed") ||
                    !entry["signed"].is_boolean())
                {
                    error = "a layout control is not {name, kind, bits, range, signed}";
                    return false;
                }
                const std::string name = entry["name"].get<std::string>();
                if (name.size() > Network::kMaxInputControlNameLength)
                {
                    error = "a layout control name is too long";
                    return false;
                }
                const auto kind = Network::ParseInputControlKind(entry["kind"].get<std::string>());
                if (!kind)
                {
                    error = "a layout control has an unknown kind";
                    return false;
                }
                const double range = entry["range"].get<double>();
                if (!std::isfinite(range) || std::fabs(range) > static_cast<double>(std::numeric_limits<float>::max()))
                {
                    error = "a layout control range is not a finite float";
                    return false;
                }
                spec.name = name;
                spec.kind = *kind;
                spec.bits = static_cast<std::uint8_t>(bits);
                spec.range = static_cast<float>(range);
                spec.isSigned = entry["signed"].get<bool>();
                specs.push_back(std::move(spec));
            }
            auto created = Network::InputLayout::Create(std::move(specs));
            if (!created)
            {
                error = "layout: " + created.error();
                return false;
            }
            layout = std::move(*created);
            // Keep the normalised form (the order of the names), so a log compares and serialises the same wherever it came from
            log.layout.clear();
            for (std::size_t i = 0; i < layout.ControlCount(); ++i)
            {
                log.layout.push_back(layout.Control(i));
            }
            if (!root.contains("layoutHash") || !root["layoutHash"].is_string() ||
                root["layoutHash"].get<std::string>() != HashText(layout.Hash()))
            {
                error = "\"layoutHash\" is missing or does not match the layout";
                return false;
            }
            return true;
        }
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
        if (log.version != kReplayFormatVersion && log.version != kReplayFormatVersionLayout)
        {
            error = "unknown version " + std::to_string(log.version) + " (this reader knows " +
                    std::to_string(kReplayFormatVersion) + " and " + std::to_string(kReplayFormatVersionLayout) + ")";
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
        const nlohmann::json &rows = root["inputs"];
        if (rows.size() > kMaxRows)
        {
            error = "too many input rows";
            return std::nullopt;
        }
        if (log.version == kReplayFormatVersion)
        {
            for (const nlohmann::json &row : rows)
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
        }
        else
        {
            Network::InputLayout layout;
            if (!ParseLayout(root, log, layout, error))
            {
                return std::nullopt;
            }
            for (const nlohmann::json &row : rows)
            {
                ReplayInput input;
                if (!row.is_array() || row.size() != 3 || !ReadInt(row[0], 0, kMaxTicks, input.tick) ||
                    !ReadInt(row[1], 0, kMaxSlots, input.slot) || !row[2].is_string() ||
                    !ParseHex(row[2].get<std::string>(), layout.PayloadBytes(), input.payload) ||
                    !DeriveSceneInput(layout, input.payload, input.moveX, input.moveY, input.buttons))
                {
                    error = "input row " + std::to_string(log.inputs.size()) +
                            " is not [tick, slot, \"<payload hex the layout produces>\"]";
                    return std::nullopt;
                }
                log.inputs.push_back(std::move(input));
            }
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
        if (log.version == kReplayFormatVersionLayout)
        {
            out += " \"layout\": [";
            for (std::size_t i = 0; i < log.layout.size(); ++i)
            {
                const Network::InputControlSpec &control = log.layout[i];
                nlohmann::json entry = nlohmann::json::object();
                entry["name"] = control.name;
                entry["kind"] = std::string(Network::ToString(control.kind));
                entry["bits"] = static_cast<int>(control.bits);
                entry["range"] = static_cast<double>(control.range);
                entry["signed"] = control.isSigned;
                out += i == 0 ? "\n  " : ",\n  ";
                out += entry.dump();
            }
            out += log.layout.empty() ? "],\n" : "\n ],\n";
            const auto layout = Network::InputLayout::Create(log.layout);
            out += " \"layoutHash\": \"" + HashText(layout ? layout->Hash() : 0) + "\",\n";
        }
        out += " \"inputs\": [";
        for (std::size_t i = 0; i < log.inputs.size(); ++i)
        {
            const ReplayInput &row = log.inputs[i];
            out += i == 0 ? "\n  [" : ",\n  [";
            if (log.version == kReplayFormatVersionLayout)
            {
                out += std::to_string(row.tick) + ", " + std::to_string(row.slot) + ", \"" + HexText(row.payload) + "\"]";
            }
            else
            {
                out += std::to_string(row.tick) + ", " + std::to_string(row.slot) + ", " + std::to_string(row.moveX) + ", " +
                       std::to_string(row.moveY) + ", " + std::to_string(row.buttons) + "]";
            }
        }
        out += log.inputs.empty() ? "]\n}\n" : "\n ]\n}\n";
        return out;
    }
}
