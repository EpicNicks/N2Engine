#pragma once

#include <memory>

#include "text/FontBackend.hpp"

namespace N2Engine::Text::Detail
{
    // Private to the text library: callers go through CreateFontBackend
    std::unique_ptr<IFontBackend> CreateStbTrueTypeBackend();
}
