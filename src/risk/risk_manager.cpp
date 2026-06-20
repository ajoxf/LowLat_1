// Anchors the risk-manager header in the core library.
#include "risk/risk_manager.hpp"

namespace hft {
// RiskManager is header-defined for hot-path inlining; this TU guarantees it
// compiles stand-alone.
}  // namespace hft
