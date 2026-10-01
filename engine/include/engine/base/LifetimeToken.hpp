#pragma once

#include <memory>

namespace N2Engine::Base
{
    /// Expires when the object holding it is freed, for references that must not dangle (e.g. from Lua):
    /// they keep a weak_ptr to it (Get) and see the object as gone once it expires.
    ///
    /// It belongs to its object, not to the object's contents: a copied or moved-to object gets a token of its
    /// own, and assignment keeps each object's token. A reference stays tied to the object it was taken from,
    /// never to another object its contents were moved into.
    class LifetimeToken
    {
    public:
        LifetimeToken() = default;
        LifetimeToken(const LifetimeToken &) {}
        LifetimeToken &operator=(const LifetimeToken &) { return *this; }

        [[nodiscard]] std::weak_ptr<const bool> Get() const { return _token; }

    private:
        std::shared_ptr<const bool> _token = std::make_shared<bool>(true);
    };
}
