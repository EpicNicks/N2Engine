#include "engine/replay/LockstepReplay.hpp"

#include <array>
#include <fstream>

#include <nlohmann/json.hpp>

namespace N2Engine::Replay
{
    LockstepRecorder::LockstepRecorder(std::string scene, const std::uint32_t seed, const int hashInterval, const int slotCount)
        : _slotCount(slotCount)
    {
        _log.scene = std::move(scene);
        _log.seed = seed;
        _log.ticks = 0;
        _log.hashInterval = hashInterval;
    }

    LockstepRecorder::LockstepRecorder(std::string scene, const std::uint32_t seed, const int hashInterval, const int slotCount,
                                       const Network::InputLayout &layout)
        : LockstepRecorder(std::move(scene), seed, hashInterval, slotCount)
    {
        _layoutMode = true;
        _layout = layout;
        _log.version = kReplayFormatVersionLayout;
        for (std::size_t i = 0; i < layout.ControlCount(); ++i)
        {
            _log.layout.push_back(layout.Control(i));
        }
        // A slot with no row holds the layout's neutral input
        for (auto &held : _heldPayload)
        {
            held = layout.NeutralPayload();
        }
    }

    bool SceneStateFromInput(const Network::InputLayout &layout, const N2Net::LockstepInput &input, SlotState &out)
    {
        return DeriveSceneInput(layout, input.Bytes(), out.moveX, out.moveY, out.buttons);
    }

    bool LockstepRecorder::RecordInputs(const std::span<const N2Net::LockstepInput> inputs)
    {
        if (!_layoutMode || _slotCount < 1 || _slotCount > kMaxSlots || _log.ticks >= kMaxTicks)
        {
            return false;
        }
        std::array<ReplayInput, kMaxSlots> rows{};
        std::array<bool, kMaxSlots> changed{};
        for (int slot = 0; slot < _slotCount; ++slot)
        {
            const auto index = static_cast<std::size_t>(slot);
            std::vector<std::uint8_t> next;
            if (index < inputs.size())
            {
                next.assign(inputs[index].Bytes().begin(), inputs[index].Bytes().end());
            }
            else
            {
                next = _layout.NeutralPayload();
            }
            ReplayInput &row = rows[index];
            if (!DeriveSceneInput(_layout, next, row.moveX, row.moveY, row.buttons))
            {
                return false;
            }
            row.tick = _log.ticks;
            row.slot = slot;
            row.payload = std::move(next);
            changed[index] = row.payload != _heldPayload[index];
        }
        for (int slot = 0; slot < _slotCount; ++slot)
        {
            const auto index = static_cast<std::size_t>(slot);
            if (changed[index])
            {
                _heldPayload[index] = rows[index].payload;
                _log.inputs.push_back(std::move(rows[index]));
            }
        }
        ++_log.ticks;
        return true;
    }

    bool LockstepRecorder::RecordTick(const std::span<const SlotState> slots)
    {
        if (_layoutMode || _slotCount < 1 || _slotCount > kMaxSlots || _log.ticks >= kMaxTicks)
        {
            return false;
        }
        const int tick = _log.ticks;
        for (int slot = 0; slot < _slotCount; ++slot)
        {
            SlotState state{};
            if (static_cast<std::size_t>(slot) < slots.size())
            {
                state = slots[static_cast<std::size_t>(slot)];
            }
            SlotState &held = _held[static_cast<std::size_t>(slot)];
            if (state.moveX != held.moveX || state.moveY != held.moveY || state.buttons != held.buttons)
            {
                _log.inputs.push_back(ReplayInput{tick, slot, state.moveX, state.moveY, state.buttons});
                held = state;
            }
        }
        ++_log.ticks;
        return true;
    }

    const char *DivergenceKindName(const N2Net::Divergence::Kind kind) noexcept
    {
        using Kind = N2Net::Divergence::Kind;
        switch (kind)
        {
        case Kind::None: return "none";
        case Kind::ObjectOnlyRemote: return "object-only-remote";
        case Kind::ObjectOnlyLocal: return "object-only-local";
        case Kind::FieldDiffers: return "field-differs";
        case Kind::FieldOnlyRemote: return "field-only-remote";
        case Kind::FieldOnlyLocal: return "field-only-local";
        case Kind::ObjectDiffersNoField: return "object-differs-no-field";
        case Kind::Malformed: return "malformed";
        }
        return "unknown";
    }

    namespace
    {
        nlohmann::json ParseOrString(const std::string &text)
        {
            if (text.empty())
            {
                return nlohmann::json::object();
            }
            nlohmann::json parsed = nlohmann::json::parse(text, nullptr, false);
            return parsed.is_discarded() ? nlohmann::json(text) : parsed;
        }
    }

    std::string DesyncReportToJson(const DesyncReport &report)
    {
        nlohmann::json root;
        root["format"] = "n2desync";
        root["version"] = 1;
        root["peer"] = report.peerName;
        root["tick"] = report.tick;
        root["localRoot"] = FormatHash(report.localRoot);
        root["remoteRoot"] = FormatHash(report.remoteRoot);
        root["build"] = ParseOrString(report.buildFingerprintJson);
        root["determinism"] = ParseOrString(report.determinismFingerprintJson);

        nlohmann::json divergence;
        divergence["kind"] = DivergenceKindName(report.divergence.kind);
        divergence["objectId"] = report.divergence.objectId;
        divergence["fieldId"] = report.divergence.fieldId;
        divergence["localHash"] = FormatHash(report.divergence.localHash);
        divergence["remoteHash"] = FormatHash(report.divergence.remoteHash);
        divergence["requests"] = report.divergence.rounds;
        root["firstDivergence"] = divergence;

        // The replay file, inline: the same object ParseReplayLog reads
        root["inputLog"] = nlohmann::json::parse(SerializeReplayLog(report.inputLog));

        nlohmann::json trees = nlohmann::json::array();
        if (report.trees != nullptr)
        {
            for (const N2Net::HashTree &tree : *report.trees)
            {
                nlohmann::json t;
                t["tick"] = tree.Tick();
                t["root"] = FormatHash(tree.Root());
                t["objectCount"] = tree.Objects().size();
                t["truncated"] = tree.Objects().size() > kMaxReportObjects;
                nlohmann::json objects = nlohmann::json::array();
                std::size_t written = 0;
                for (const N2Net::HashTree::Object &object : tree.Objects())
                {
                    if (written++ >= kMaxReportObjects)
                    {
                        break;
                    }
                    nlohmann::json o;
                    o["id"] = object.id;
                    o["hash"] = FormatHash(object.hash);
                    nlohmann::json fields = nlohmann::json::array();
                    for (const N2Net::HashLeaf &leaf : object.fields)
                    {
                        fields.push_back(nlohmann::json::array({leaf.fieldId, FormatHash(leaf.hash)}));
                    }
                    o["fields"] = fields;
                    objects.push_back(o);
                }
                t["objects"] = objects;
                trees.push_back(t);
            }
        }
        root["trees"] = trees;
        return root.dump(1) + "\n";
    }

    bool WriteDesyncReport(const DesyncReport &report, const std::string &path, std::string &error)
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        if (!out)
        {
            error = "cannot write " + path;
            return false;
        }
        out << DesyncReportToJson(report);
        out.flush();
        if (!out)
        {
            error = "write to " + path + " failed";
            return false;
        }
        return true;
    }
}
