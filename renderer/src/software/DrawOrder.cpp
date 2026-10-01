#include "renderer/software/DrawOrder.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace Renderer::Software
{
    std::vector<std::size_t> OrderDraws(const std::span<const DrawOrderKey> draws)
    {
        std::vector<std::size_t> order(draws.size());
        for (std::size_t i = 0; i < order.size(); ++i)
        {
            order[i] = i;
        }

        // NaN would break the comparator's strict weak ordering; sort it last instead
        auto distance = [&](const std::size_t index)
        {
            const float d = draws[index].distanceSq;
            return std::isnan(d) ? std::numeric_limits<float>::infinity() : d;
        };

        std::size_t runStart = 0;
        while (runStart < order.size())
        {
            if (!IsReorderable(draws[runStart].state))
            {
                ++runStart;
                continue;
            }

            std::size_t runEnd = runStart + 1;
            while (runEnd < order.size() && IsReorderable(draws[runEnd].state))
            {
                ++runEnd;
            }

            const auto first = order.begin() + static_cast<std::ptrdiff_t>(runStart);
            const auto last = order.begin() + static_cast<std::ptrdiff_t>(runEnd);
            std::stable_sort(first, last,
                             [&](const std::size_t a, const std::size_t b) { return distance(a) < distance(b); });

            runStart = runEnd;
        }

        return order;
    }
}
