#pragma once

#include <cstddef>
#include <span>

namespace N2Engine::Text
{
    /// The bytes of the built-in font (Noto Sans Regular, Latin subset; engine/assets/fonts/README.md),
    /// embedded at build time. Valid for the whole program.
    [[nodiscard]] std::span<const std::byte> GetDefaultFontData();
}
