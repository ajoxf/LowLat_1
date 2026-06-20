// Anchors the feed-handler headers in the core library.
#include "feed_handler/feed_handler.hpp"

#include "feed_handler/mtbt_parser.hpp"

namespace hft {
// FeedHandler / MtbtParser are header-defined for hot-path inlining; this TU
// guarantees they compile stand-alone.
}  // namespace hft
