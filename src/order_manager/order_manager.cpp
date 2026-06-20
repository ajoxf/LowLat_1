// Anchors the order-manager, NNF encoder and NNF session headers in the core
// library.
#include "order_manager/order_manager.hpp"

#include "order_manager/nnf_encoder.hpp"
#include "session/nnf_session.hpp"

namespace hft {
// These components are header-defined for hot-path inlining; this TU guarantees
// they compile stand-alone.
}  // namespace hft
