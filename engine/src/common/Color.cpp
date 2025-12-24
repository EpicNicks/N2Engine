#include "engine/common/Color.hpp"
#include <cstdint>

using namespace N2Engine::Common;
using namespace N2Engine::Math;

Color& Color::operator=(const Color &other)
{
    if (this != &other)
    {
        Vector4::operator=(other);
    }
    return *this;
}

const Color Color::White{1.0f, 1.0f, 1.0f, 1.0f};
const Color Color::Black{0.0f, 0.0f, 0.0f, 1.0f};
const Color Color::Red{1.0f, 0.0f, 0.0f, 1.0f};
const Color Color::Green{0.0f, 1.0f, 0.0f, 1.0f};
const Color Color::Blue{0.0f, 0.0f, 1.0f, 1.0f};
const Color Color::Cyan{0.0f, 1.0f, 1.0f, 1.0f};
const Color Color::Yellow{1.0f, 1.0f, 0.0f, 1.0f};
const Color Color::Magenta{1.0f, 0.0f, 1.0f, 1.0f};
const Color Color::Transparent{0.0f, 0.0f, 0.0f, 0.0f};

Color Color::FromHex(std::uint32_t hexValue)
{
    float r = ((hexValue >> 24) & 0xFF) / 255.0f;
    float g = ((hexValue >> 16) & 0xFF) / 255.0f;
    float b = ((hexValue >> 8) & 0xFF) / 255.0f;
    float a = (hexValue & 0xFF) / 255.0f;
    return Color{r, g, b, a};
}

std::uint32_t Color::ToHex() const
{
    std::uint8_t red = static_cast<std::uint8_t>(std::min(255.0f, std::max(0.0f, r * 255.0f + 0.5f)));
    std::uint8_t green = static_cast<std::uint8_t>(std::min(255.0f, std::max(0.0f, g * 255.0f + 0.5f)));
    std::uint8_t blue = static_cast<std::uint8_t>(std::min(255.0f, std::max(0.0f, b * 255.0f + 0.5f)));
    std::uint8_t alpha = static_cast<std::uint8_t>(std::min(255.0f, std::max(0.0f, a * 255.0f + 0.5f)));

    return (static_cast<std::uint32_t>(red) << 24) |
        (static_cast<std::uint32_t>(green) << 16) |
        (static_cast<std::uint32_t>(blue) << 8) |
        static_cast<std::uint32_t>(alpha);
}
