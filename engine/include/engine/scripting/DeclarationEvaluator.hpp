#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "engine/io/ResourcePath.hpp"
#include "engine/scripting/LuaFieldDeclaration.hpp"
#include "engine/serialization/FieldInfo.hpp"

namespace N2Engine::Scripting
{
    /// What one evaluation may spend. The defaults are the editor's (design: 1,000,000 VM instructions and 16 MiB per
    /// script, source at most 4 MiB, at most 256 fields, strings in defaults up to 4 KiB); tests lower them.
    struct DeclarationLimits
    {
        /// Lua VM instructions the whole evaluation (the chunk and the reading of its declarations) may run (0 means
        /// the default, never unlimited)
        std::uint64_t instructions = 1'000'000;
        /// Bytes the throwaway state may hold (at least 1 MiB is given; 0 means the default, never unlimited)
        std::size_t maxBytes = std::size_t{16} * 1024 * 1024;
        /// A longer source is refused before any state is made (the text-asset cap)
        std::size_t maxSourceBytes = std::size_t{4} * 1024 * 1024;
        /// Fields read from SerializableFields; the rest are ignored (an error says so)
        std::size_t maxFields = 256;
        /// A string default longer than this is not read (that field's default is unevaluable)
        std::size_t maxStringBytes = 4096;
        /// Wall-clock milliseconds the whole evaluation may take (0 means the default, never unlimited). The
        /// instruction budget does not bound time: one instruction can compare or copy megabytes. Reported as
        /// budgetExceeded with timedOut set.
        std::uint64_t maxMillis = 2000;
    };

    /// One entry of a script's SerializableFields as the evaluator read it
    struct DeclaredField
    {
        std::string name;
        /// `{ type = "...", default = ... }` rather than the shorthand `speed = 5`
        bool isDefinition = false;
        /// The entry's `type` string; empty when it has none, or it is not a string
        std::string declaredType;
        /// What the default is (the shorthand value is its own default); what DescribeFields classifies it as
        LuaDefaultKind defaultKind = LuaDefaultKind::Other;
        /// The default as ExtractSerializableFields would store it in scriptData (an integer, a float, a bool, a
        /// string, {x,y,z}); null when there is none or it could not be read
        nlohmann::json defaultValue;
        /// The type or default of this field comes from the engine (an engine call at the script's top level), which
        /// the evaluation has no engine to answer: its real value is unknown here. The field is still listed, with
        /// the kind the rest of its declaration gives (Json when nothing else does).
        bool unevaluable = false;
        std::string unevaluableReason;
    };

    /// What DeclarationEvaluator::Evaluate found. Every way an evaluation can go wrong is data here; none throws.
    struct DeclarationResult
    {
        /// The script returned a table whose SerializableFields is a table (an empty one counts)
        bool declared = false;
        /// The entries of SerializableFields keyed by a string, sorted by name (as DescribeFields lists them)
        std::vector<DeclaredField> fields;
        /// Why the evaluation failed or was cut short: a syntax or runtime error of the script, "Script must return a
        /// table", an error of reading SerializableFields, a refused __gc, more fields than the limit, a budget or memory
        /// failure. Texts are sanitised and short.
        std::vector<std::string> errors;
        /// The script ran past the instruction budget (also in errors)
        bool budgetExceeded = false;
        /// The wall-clock limit (not the instruction count) was what ended it; budgetExceeded is set too
        bool timedOut = false;
        /// The script (or the reading of its declarations) ran into the memory cap (also in errors)
        bool memoryExhausted = false;
        /// The list of fields itself cannot be known: the script returns a value that came from the engine, or its
        /// SerializableFields does. Never an empty list standing for "unknown".
        bool unevaluable = false;
        std::string unevaluableReason;
        /// How many times the script used an engine placeholder (read a global that is not there, called or indexed
        /// one): 0 means the script touched nothing of the engine at its top level
        std::uint64_t stubUses = 0;
        /// The most bytes the throwaway state held, and the bytes still counted after it was closed (0: nothing leaked)
        std::size_t peakBytes = 0;
        std::size_t leakedBytes = 0;

        /// Nothing went wrong and nothing is unknown: the fields are what a real run would declare, as far as the
        /// evaluation can tell (a script that branches on an engine value cannot be told apart: see the docs)
        [[nodiscard]] bool Complete() const;
        /// Some field's type or default is unknown
        [[nodiscard]] bool AnyFieldUnevaluable() const;
        /// The script's fields as LuaComponent::DescribeFields lists them after its `scriptUUID` entry
        [[nodiscard]] std::vector<FieldInfo> ToFieldInfos() const;
        /// The defaults ExtractSerializableFields would put in scriptData: an object, one key per field with a default
        [[nodiscard]] nlohmann::json DefaultValues() const;
    };

    /// Reads the field declarations of a Lua script WITHOUT running it in the engine: the source is run once in a
    /// throwaway Lua state of its own (a fresh allocator with a byte cap, an instruction budget, the engine's sandbox
    /// minus every engine binding), SerializableFields is read out as plain data, and the state is closed. Nothing of
    /// the run persists and no engine object, singleton or Lua state (LuaRuntime) is touched, so it is safe to call
    /// from any thread, from several at once, and before or without a LuaRuntime. Not wired into the editor host yet.
    ///
    /// Global names the script reads that are not there (Application, Input, Debug, GameObject, Math, ...) answer an
    /// inert placeholder: callable, indexable, usable in arithmetic, always returning itself, so a top-level
    /// `Debug.Log("x")` or `local g = GameObject.Find("y")` does not stop the class from being defined. docs/scripting-lua.html
    /// lists exactly what differs from a real run.
    class DeclarationEvaluator
    {
    public:
        /// The longest length or range a table library call (insert, remove, concat, unpack, move, sort)
        /// may use during an evaluation (2^16; no declaration needs more). A longer one ends the evaluation as
        /// budgetExceeded: those functions loop in C, where the instruction budget and the time limit cannot stop them.
        static constexpr std::int64_t MaxTableLoop = 65536;

        /// @param source the script's text (binary chunks are refused)
        /// @param chunkName names it in error messages
        [[nodiscard]] static DeclarationResult Evaluate(std::string_view source, const std::string &chunkName,
                                                         const DeclarationLimits &limits = {});

        /// Evaluate for a script asset: loaded through ResourceLoader (which is as thread-safe as it is: evaluate
        /// from several threads with Evaluate), a script that cannot be loaded is an error in the result
        [[nodiscard]] static DeclarationResult EvaluateScript(const IO::ResourcePath &path,
                                                               const DeclarationLimits &limits = {});

        /// Throwaway states that exist right now: 0 whenever no Evaluate is running (every one is closed on every
        /// path). For tests.
        [[nodiscard]] static int LiveStates();
    };
}
