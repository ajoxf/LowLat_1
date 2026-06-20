// Anchors the strategy headers in the core library.
#include "strategy/market_making.hpp"

#include "strategy/strategy_base.hpp"

namespace hft {
// MarketMaking is header-defined for hot-path inlining; this TU guarantees the
// strategy headers compile stand-alone.
}  // namespace hft
