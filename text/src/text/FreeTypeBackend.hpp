#pragma once

#include <memory>

#include "text/FontBackend.hpp"

namespace N2Engine::Text::Detail
{
    // Private to the text library: callers go through CreateFontBackend. Only defined when the build has
    // N2ENGINE_TEXT_FREETYPE (FreeTypeBackend.cpp is left out of the build otherwise).
    std::unique_ptr<IFontBackend> CreateFreeTypeBackend();
}
