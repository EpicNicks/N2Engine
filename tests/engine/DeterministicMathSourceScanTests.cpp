#include <gtest/gtest.h>

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

// Netcode C1: no C library transcendental function in a simulation path. This reads the engine's and the math library's own
// sources as text and fails on any std::sin, cos, tan, asin, acos, atan, atan2, exp, log, pow and their relatives that is
// not in the list below. The list is the audit: every file that keeps a C library call, and why it may. A new call anywhere
// else must either use Math::Sin and the rest (math/DeterministicMath.hpp) or be added here with its reason.
//
// Not scanned: renderer/ (drawing only, never read by the simulation), text/, assetimport/, editor-server/, tests/.

namespace fs = std::filesystem;

namespace
{
    struct Allowed
    {
        const char *file; // relative to the engine source directory
        const char *reason;
    };

    const Allowed kAllowed[] = {
        {"src/Camera.cpp", "std::tan of the field of view: the projection matrix, drawing and picking rays only"},
        {"src/rendering/Mesh.cpp", "std::sin and std::cos generating the vertices of the built-in sphere: rendering geometry"},
        {"src/particles/ParticleEmitter.cpp",
         "std::sin, std::cos and std::cbrt placing cosmetic particles: they are not part of the simulation state, which "
         "gameplay never reads, and an emitter has its own seeded generator for one machine"},
    };

    std::string ReadFile(const fs::path &path)
    {
        std::ifstream in(path, std::ios::binary);
        std::ostringstream text;
        text << in.rdbuf();
        return text.str();
    }

    // Comments and string literals removed, so prose that mentions std::sin does not count
    std::string StripCommentsAndStrings(const std::string &source)
    {
        std::string out;
        out.reserve(source.size());
        for (std::size_t i = 0; i < source.size(); ++i)
        {
            const char c = source[i];
            if (c == '/' && i + 1 < source.size() && source[i + 1] == '/')
            {
                while (i < source.size() && source[i] != '\n')
                {
                    ++i;
                }
                out.push_back('\n');
            }
            else if (c == '/' && i + 1 < source.size() && source[i + 1] == '*')
            {
                i += 2;
                while (i + 1 < source.size() && !(source[i] == '*' && source[i + 1] == '/'))
                {
                    if (source[i] == '\n')
                    {
                        out.push_back('\n');
                    }
                    ++i;
                }
                ++i;
            }
            else if (c == '"')
            {
                ++i;
                while (i < source.size() && source[i] != '"')
                {
                    if (source[i] == '\\')
                    {
                        ++i;
                    }
                    ++i;
                }
                out += "\"\"";
            }
            else
            {
                out.push_back(c);
            }
        }
        return out;
    }

    // "file: call" for every std:: or unqualified call of a C library transcendental function. A member call (a.log(),
    // p->exp()) or another scope's function (Foo::sin()) is not the C library's and is not matched.
    std::vector<std::string> Offenders(const fs::path &root)
    {
        static const std::regex call(
            "(^|[^A-Za-z0-9_.:>])((?:std::)?(?:sin|cos|tan|asin|acos|atan|atan2|sinh|cosh|tanh|exp|exp2|expm1|log|log2|"
            "log10|log1p|pow|cbrt|hypot|sinf|cosf|tanf|asinf|acosf|atanf|atan2f|expf|logf|powf))\\s*\\(");
        std::vector<std::string> found;
        for (const fs::directory_entry &entry : fs::recursive_directory_iterator(root))
        {
            if (!entry.is_regular_file())
            {
                continue;
            }
            const std::string extension = entry.path().extension().string();
            if (extension != ".cpp" && extension != ".hpp" && extension != ".h" && extension != ".inl")
            {
                continue;
            }
            const std::string code = StripCommentsAndStrings(ReadFile(entry.path()));
            const std::string relative = fs::relative(entry.path(), root).generic_string();
            for (std::sregex_iterator it(code.begin(), code.end(), call), end; it != end; ++it)
            {
                found.push_back(relative + ": " + (*it)[2].str());
            }
        }
        return found;
    }

    std::string FileOf(const std::string &offender)
    {
        return offender.substr(0, offender.find(':'));
    }
}

TEST(DeterministicMathSourceScanTest, TheMathLibraryCallsNoCLibraryTranscendentalFunction)
{
    const fs::path root = N2_MATH_SOURCE_DIR;
    ASSERT_TRUE(fs::exists(root)) << root;
    for (const std::string &offender : Offenders(root))
    {
        ADD_FAILURE() << "math/" << offender << ": use Math::Sin, Math::Cos, ... (math/DeterministicMath.hpp)";
    }
}

TEST(DeterministicMathSourceScanTest, EngineSourcesCallNoCLibraryTranscendentalFunctionOutsideTheAuditedList)
{
    const fs::path root = N2_ENGINE_SOURCE_DIR;
    ASSERT_TRUE(fs::exists(root)) << root;
    std::set<std::string> allowed;
    for (const Allowed &entry : kAllowed)
    {
        allowed.insert(entry.file);
        EXPECT_TRUE(fs::exists(root / entry.file)) << "the audited list names a file that does not exist: " << entry.file;
    }
    for (const std::string &offender : Offenders(root))
    {
        if (allowed.count(FileOf(offender)) == 0)
        {
            ADD_FAILURE() << "engine/" << offender
                          << ": use Math::Sin, Math::Cos, ... (math/DeterministicMath.hpp), or add the file to the audited "
                             "list in DeterministicMathSourceScanTests.cpp with the reason it cannot affect the simulation";
        }
    }
}

// The audited files really do still contain a call: a stale entry is removed, not left to hide a later regression
TEST(DeterministicMathSourceScanTest, EveryAuditedFileStillNeedsItsEntry)
{
    const fs::path root = N2_ENGINE_SOURCE_DIR;
    std::set<std::string> filesWithCalls;
    for (const std::string &offender : Offenders(root))
    {
        filesWithCalls.insert(FileOf(offender));
    }
    for (const Allowed &entry : kAllowed)
    {
        EXPECT_EQ(filesWithCalls.count(entry.file), 1u)
            << entry.file << " no longer calls a C library transcendental function: remove its entry";
    }
}
