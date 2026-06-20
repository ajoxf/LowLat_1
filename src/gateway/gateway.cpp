// Anchors the gateway headers in the core library.
#include "gateway/market_data_gateway.hpp"

#include "gateway/order_gateway.hpp"
#include "gateway/udp_socket.hpp"

namespace hft {
// Gateways are header-defined for hot-path inlining; this TU guarantees they
// compile stand-alone.
}  // namespace hft
