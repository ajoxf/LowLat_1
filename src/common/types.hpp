// Core fundamental types for the NSE HFT system.
//
// NSE conventions are used verbatim so that this code reads the same way the
// NSE protocol documents do:
//   * Instruments are identified by a numeric "Token", never a string symbol.
//   * Prices are fixed point in paisa (rupees * 100) -- never floating point.
//   * Byte ordering on the wire is little-endian.
#ifndef HFT_COMMON_TYPES_HPP_
#define HFT_COMMON_TYPES_HPP_

#include <cstdint>

namespace hft {

// NSE identifies instruments by a numeric token (from the symbol master CSV),
// assigned per instrument, per segment, per expiry.
using Token = uint32_t;

// Price in fixed point. NSE sends price * 100 (paisa) for both CM and FO.
// We store it exactly as received -- never convert to a float.
using Price = int64_t;

// Number of shares (CM) or contracts (FO). NSE uses a signed integer.
using Quantity = int32_t;

// NSE order number, assigned by the exchange on acknowledgement.
using OrderId = int64_t;

// Our internal client order id. We assign it before sending and NSE echoes it
// back in every response for correlation.
using ClOrdId = int64_t;

// Nanoseconds since the Unix epoch, IST-aware (see time_utils.hpp).
using Timestamp = uint64_t;

// Order side. true == BUY, false == SELL.
using Side = bool;

inline constexpr Side kBuy = true;
inline constexpr Side kSell = false;

// Trading segment.
using Segment = uint8_t;

inline constexpr Segment kSegmentCm = 1;  // Capital Market.
inline constexpr Segment kSegmentFo = 2;  // Futures & Options.
inline constexpr Segment kSegmentCd = 3;  // Currency Derivatives.

// ---------------------------------------------------------------------------
// Market data
// ---------------------------------------------------------------------------

// NSE MTBT market-data event, decoded from a single MTBT message.
struct MarketEvent {
  Timestamp ts;          // Exchange timestamp from the MTBT packet header (ns).
  Token token;           // NSE instrument token.
  Price price;           // Fixed point, in paisa.
  Quantity qty;          // Quantity.
  Side side;             // BUY or SELL.
  uint8_t type;          // MtbtMsgType value (ORDER_ADD, ..., TRADE, ...).
  int64_t order_no;      // NSE order number carried by the message.
  uint32_t stream_id;    // MTBT stream id (1..N).
  uint64_t seq_no;       // Per-stream MTBT sequence number (resets daily).

  // Trade ('T') only: the two resting order numbers and the trade number.
  int64_t order_no2;     // Second resting order number (sell side on a trade).
  int64_t trade_no;      // NSE trade number for reconciliation.

  // Snap Quote ('Q') only: both sides of top-of-book.
  Price bid_price;
  Quantity bid_qty;
  Price ask_price;
  Quantity ask_qty;
};

// ---------------------------------------------------------------------------
// Orders
// ---------------------------------------------------------------------------

// NSE order types.
enum class OrderType : uint8_t {
  kLimit = 1,
  kMarket = 2,
  kStopLoss = 3,         // SL: limit triggered at a stop price.
  kStopLossMarket = 4,   // SL-M: market triggered at a stop price.
};

// NSE NNF order validity types.
enum class Validity : uint8_t {
  kDay = 1,   // Good for the day.
  kIoc = 3,   // Immediate or cancel.
  kGtd = 4,   // Good till date (FO only).
  kEos = 6,   // End of session.
};

// An order we want to send to NSE via the NNF protocol.
struct Order {
  ClOrdId client_order_id;    // Our id -- assigned before send, echoed back.
  int64_t exchange_order_no;  // NSE's id -- 0 until acknowledged.
  Token token;
  Segment segment;
  Price price;                // In paisa.
  Quantity qty;
  Quantity disclosed_qty;     // Disclosed / iceberg quantity (0 == standard).
  Side side;
  OrderType order_type;
  Validity validity;
  Timestamp created_at;
};

// A fill / trade confirmation received from NSE.
struct FillReport {
  int64_t exchange_order_no;
  ClOrdId client_order_id;
  Token token;
  Price fill_price;       // In paisa.
  Quantity fill_qty;
  Quantity remaining_qty;
  Timestamp fill_time;
  int64_t trade_no;       // NSE trade number for reconciliation.
  Side side;
};

// ---------------------------------------------------------------------------
// MTBT message type codes (MTBT spec v6.3).
// ---------------------------------------------------------------------------
enum class MtbtMsgType : uint8_t {
  kOrderAdd = 'A',
  kOrderModify = 'M',
  kOrderCancel = 'X',
  kTrade = 'T',
  kOpenInterest = 'O',
  kSnapQuote = 'Q',
  kHeartbeat = 'H',
};

}  // namespace hft

#endif  // HFT_COMMON_TYPES_HPP_
