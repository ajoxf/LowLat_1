// Translation unit anchoring the order-book headers in the core library and
// verifying they compile stand-alone.
#include "order_book/order_book.hpp"

#include "order_book/book_manager.hpp"

namespace hft {
// Intentionally empty: OrderBook / BookManager are fully defined in headers for
// hot-path inlining. This TU guarantees they compile in isolation.
}  // namespace hft
