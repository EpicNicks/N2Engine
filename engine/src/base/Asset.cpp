#include "engine/base/Asset.hpp"

using namespace N2Engine;
using namespace N2Engine::Math;
using namespace N2Engine::Base;

UUID Asset::GetUUID() const
{
    return _uuid;
}

void Asset::SetUUID(const UUID &uuid)
{
    _uuid = uuid;
}

Asset::Asset() : _uuid{UUID::Random()} {}

Asset::Asset(UUID uuid) : _uuid{uuid} {}