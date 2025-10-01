#include "engine/base/Asset.hpp"

using namespace N2Engine;
using namespace N2Engine::Math;
using namespace N2Engine::Base;

UUID Asset::GetUUID()
{
    return _uuid;
}

Asset::Asset() : _uuid{} {}

Asset::Asset(UUID uuid) : _uuid{uuid} {}
