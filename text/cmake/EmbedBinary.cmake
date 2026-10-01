# Writes a C++ source that embeds a binary file. Run by add_custom_command (cmake -P), so the file is
# read at build time and the generated source is never committed.
#   -DINPUT=<file to embed>  -DOUTPUT=<.cpp to write>
#   -DNAMESPACE=<C++ namespace>  -DFUNCTION=<name of the std::span<const std::byte>() accessor>
foreach(variable INPUT OUTPUT NAMESPACE FUNCTION)
    if(NOT DEFINED ${variable})
        message(FATAL_ERROR "EmbedBinary.cmake: ${variable} is not set")
    endif()
endforeach()

file(READ "${INPUT}" hex HEX)
string(LENGTH "${hex}" hexLength)
if(hexLength EQUAL 0)
    message(FATAL_ERROR "EmbedBinary.cmake: ${INPUT} is empty")
endif()
math(EXPR byteCount "${hexLength} / 2")

# 32 bytes per line keeps the lines short (CMake regexes have no {n}, hence the repeated class), then each
# byte pair becomes "0x..,"
string(REPEAT "[0-9a-f]" 64 lineOfHex)
string(REGEX REPLACE "(${lineOfHex})" "\\1\n" hex "${hex}")
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," bytes "${hex}")

get_filename_component(inputName "${INPUT}" NAME)
file(WRITE "${OUTPUT}" "// Generated at build time by text/cmake/EmbedBinary.cmake from ${inputName}. Do not edit or commit.
#include <cstddef>
#include <span>

namespace
{
    // ${byteCount} bytes
    alignas(16) const unsigned char kEmbeddedData[] = {
${bytes}
    };
}

namespace ${NAMESPACE}
{
    std::span<const std::byte> ${FUNCTION}()
    {
        return std::as_bytes(std::span<const unsigned char>(kEmbeddedData));
    }
}
")
