#include "engine/base/Asset.hpp"

using namespace N2Engine;
using namespace N2Engine::Math;

UUID Asset::GetUUID()
{
    return _uuid;
}

Asset::Asset() : _uuid{} {}
