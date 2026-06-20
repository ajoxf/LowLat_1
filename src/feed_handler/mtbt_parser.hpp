// NSE MTBT (Multicast Tick-By-Tick) binary parser.
//
// IMPORTANT: NSE's MTBT wire format is proprietary and distributed only to
// members. The byte layouts below are a faithful, representative model of MTBT
// v6.3 (little-endian, numeric tokens, batched messages, fixed-point paisa
// prices). When integrating against the live feed, treat NSE's MTBT spec as the
// ground truth and adjust the offsets/sizes here to match exactly.
//
// Parsing is allocation-free and reads each field with an explicit
// little-endian load so the result is correct on any host endianness.
#ifndef HFT_FEED_HANDLER_MTBT_PARSER_HPP_
#define HFT_FEED_HANDLER_MTBT_PARSER_HPP_

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "common/compiler.hpp"
#include "common/types.hpp"

namespace hft {

// Little-endian load of an unsigned integer of width sizeof(T) from raw bytes.
template <typename T>
HFT_ALWAYS_INLINE T load_le(const uint8_t* p) {
  static_assert(sizeof(T) <= 8, "load_le supports up to 64-bit");
  uint64_t v = 0;
  for (size_t i = 0; i < sizeof(T); ++i) {
    v |= static_cast<uint64_t>(p[i]) << (8 * i);
  }
  return static_cast<T>(v);
}

// Signed loads via two's-complement bit reinterpretation.
HFT_ALWAYS_INLINE int32_t load_le_i32(const uint8_t* p) {
  uint32_t u = load_le<uint32_t>(p);
  int32_t s;
  std::memcpy(&s, &u, sizeof(s));
  return s;
}
HFT_ALWAYS_INLINE int64_t load_le_i64(const uint8_t* p) {
  uint64_t u = load_le<uint64_t>(p);
  int64_t s;
  std::memcpy(&s, &u, sizeof(s));
  return s;
}

// MTBT packet header layout (offsets in bytes).
//   stream_id : uint16 @ 0
//   seq_no    : uint64 @ 2
//   msg_count : uint8  @ 10
//   timestamp : uint64 @ 11  (exchange ns, IST)
inline constexpr size_t kMtbtHeaderSize = 19;

struct MtbtHeader {
  uint16_t stream_id;
  uint64_t seq_no;
  uint8_t msg_count;
  uint64_t timestamp;
};

// Per-message body sizes INCLUDING the 1-byte type tag.
inline constexpr size_t kSizeOrderAdd = 26;     // A
inline constexpr size_t kSizeOrderModify = 25;  // M
inline constexpr size_t kSizeOrderCancel = 13;  // X
inline constexpr size_t kSizeTrade = 41;        // T
inline constexpr size_t kSizeSnapQuote = 29;    // Q
inline constexpr size_t kSizeHeartbeat = 1;     // H

// Buy/sell indicator on the wire: 1 = buy, 2 = sell.
HFT_ALWAYS_INLINE Side decode_side(uint8_t indicator) { return indicator == 1 ? kBuy : kSell; }

class MtbtParser {
 public:
  // Decode the packet header. Returns false if the buffer is too short.
  HFT_ALWAYS_INLINE bool parse_header(const uint8_t* data, size_t len, MtbtHeader* out) const {
    if (len < kMtbtHeaderSize) {
      return false;
    }
    out->stream_id = load_le<uint16_t>(data + 0);
    out->seq_no = load_le<uint64_t>(data + 2);
    out->msg_count = load_le<uint8_t>(data + 10);
    out->timestamp = load_le<uint64_t>(data + 11);
    return true;
  }

  // Expected on-wire size of a message given its leading type byte. Returns 0
  // for an unknown type (caller must then stop -- size is indeterminate).
  HFT_ALWAYS_INLINE static size_t message_size(uint8_t type) {
    switch (static_cast<MtbtMsgType>(type)) {
      case MtbtMsgType::kOrderAdd:
        return kSizeOrderAdd;
      case MtbtMsgType::kOrderModify:
        return kSizeOrderModify;
      case MtbtMsgType::kOrderCancel:
        return kSizeOrderCancel;
      case MtbtMsgType::kTrade:
        return kSizeTrade;
      case MtbtMsgType::kSnapQuote:
        return kSizeSnapQuote;
      case MtbtMsgType::kHeartbeat:
        return kSizeHeartbeat;
      default:
        return 0;
    }
  }

  // Decode a single message body into `out`. `data` points at the type byte;
  // `len` is the bytes available. Returns the number of bytes consumed, or 0 on
  // a truncated/unknown message (the caller logs a WARNING and stops the batch).
  // `seq_no`/`ts`/`stream_id` are propagated from the packet header.
  size_t parse_message(const uint8_t* data, size_t len, uint16_t stream_id, uint64_t seq_no,
                       uint64_t ts, MarketEvent* out) const {
    if (len < 1) {
      return 0;
    }
    const uint8_t type = data[0];
    const size_t need = message_size(type);
    if (need == 0 || len < need) {
      return 0;  // Unknown or truncated.
    }

    out->type = type;
    out->stream_id = stream_id;
    out->seq_no = seq_no;
    out->ts = ts;
    out->token = 0;
    out->price = 0;
    out->qty = 0;
    out->order_no = 0;
    out->order_no2 = 0;
    out->trade_no = 0;
    out->side = kBuy;
    out->bid_price = 0;
    out->bid_qty = 0;
    out->ask_price = 0;
    out->ask_qty = 0;

    switch (static_cast<MtbtMsgType>(type)) {
      case MtbtMsgType::kOrderAdd: {
        // order_no(i64)@1, side(u8)@9, qty(i32)@10, token(u32)@14, price(i64)@18
        out->order_no = load_le_i64(data + 1);
        out->side = decode_side(load_le<uint8_t>(data + 9));
        out->qty = load_le_i32(data + 10);
        out->token = load_le<uint32_t>(data + 14);
        out->price = load_le_i64(data + 18);
        break;
      }
      case MtbtMsgType::kOrderModify: {
        // order_no(i64)@1, qty(i32)@9, token(u32)@13, price(i64)@17
        out->order_no = load_le_i64(data + 1);
        out->qty = load_le_i32(data + 9);
        out->token = load_le<uint32_t>(data + 13);
        out->price = load_le_i64(data + 17);
        break;
      }
      case MtbtMsgType::kOrderCancel: {
        // order_no(i64)@1, token(u32)@9
        out->order_no = load_le_i64(data + 1);
        out->token = load_le<uint32_t>(data + 9);
        break;
      }
      case MtbtMsgType::kTrade: {
        // buy_order_no(i64)@1, sell_order_no(i64)@9, token(u32)@17,
        // price(i64)@21, qty(i32)@29, trade_no(i64)@33
        out->order_no = load_le_i64(data + 1);   // Buy (resting) order number.
        out->order_no2 = load_le_i64(data + 9);  // Sell (resting) order number.
        out->token = load_le<uint32_t>(data + 17);
        out->price = load_le_i64(data + 21);
        out->qty = load_le_i32(data + 29);
        out->trade_no = load_le_i64(data + 33);
        break;
      }
      case MtbtMsgType::kSnapQuote: {
        // token(u32)@1, bid_price(i64)@5, bid_qty(i32)@13,
        // ask_price(i64)@17, ask_qty(i32)@25
        out->token = load_le<uint32_t>(data + 1);
        out->bid_price = load_le_i64(data + 5);
        out->bid_qty = load_le_i32(data + 13);
        out->ask_price = load_le_i64(data + 17);
        out->ask_qty = load_le_i32(data + 25);
        break;
      }
      case MtbtMsgType::kHeartbeat:
        break;  // Nothing but the type tag; timestamp came from the header.
      default:
        return 0;
    }
    return need;
  }

  // Parse a whole UDP packet (header + batched messages), invoking emit() for
  // each decoded MarketEvent. Returns the number of messages emitted. Unknown
  // or truncated messages stop the batch (and bump error stats).
  template <typename EmitFn>
  size_t parse_packet(const uint8_t* data, size_t len, EmitFn&& emit) {
    MtbtHeader hdr;
    if (!parse_header(data, len, &hdr)) {
      ++malformed_;
      return 0;
    }
    size_t off = kMtbtHeaderSize;
    size_t emitted = 0;
    for (uint8_t i = 0; i < hdr.msg_count; ++i) {
      MarketEvent ev;
      // Each batched message carries the packet's base seq + its index so the
      // book sees a unique, contiguous sequence number per message.
      const size_t consumed = parse_message(data + off, len - off, hdr.stream_id,
                                             hdr.seq_no + i, hdr.timestamp, &ev);
      if (consumed == 0) {
        ++malformed_;
        break;
      }
      // Heartbeats are health signals, not book events.
      if (static_cast<MtbtMsgType>(ev.type) != MtbtMsgType::kHeartbeat) {
        emit(ev);
        ++emitted;
      }
      ++parsed_;
      off += consumed;
    }
    return emitted;
  }

  uint64_t parsed() const { return parsed_; }
  uint64_t malformed() const { return malformed_; }
  void reset_stats() {
    parsed_ = 0;
    malformed_ = 0;
  }

 private:
  uint64_t parsed_ = 0;
  uint64_t malformed_ = 0;
};

}  // namespace hft

#endif  // HFT_FEED_HANDLER_MTBT_PARSER_HPP_
