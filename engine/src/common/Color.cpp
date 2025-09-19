#include "engine/common/Color.hpp"
#include <cstdint>

using namespace N2Engine::Common;
using namespace N2Engine::Math;

Color &Color::operator=(const Color &other)
{
    if (this != &other)
    {
        VectorN<float, 4>::operator=(other);
    }
    return *this;
}

Color Color::White()
{
    return Color{1.0f, 1.0f, 1.0f, 1.0f};
}
Color Color::Black()
{
    return Color{0.0f, 0.0f, 0.0f, 1.0f};
}
Color Color::Red()
{
    return Color{1.0f, 0.0f, 0.0f, 1.0f};
}
Color Color::Green()
{
    return Color{0.0f, 1.0f, 0.0f, 1.0f};
}
Color Color::Blue()
{
    return Color{0.0f, 0.0f, 1.0f, 1.0f};
}
Color Color::Cyan()
{
    return Color{0.0f, 1.0f, 1.0f, 1.0f};
}
Color Color::Yellow()
{
    return Color{1.0f, 1.0f, 0.0f, 1.0f};
}
Color Color::Magenta()
{
    return Color{1.0f, 0.0f, 1.0f, 1.0f};
}
Color Color::Transparent()
{
    return Color{0.0f, 0.0f, 0.0f, 0.0f};
}
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
