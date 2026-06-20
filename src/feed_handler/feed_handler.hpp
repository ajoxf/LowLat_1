// Dual-source NSE MTBT feed handler.
//
// NSE disseminates every stream on two multicast sources (Source 1 and
// Source 2) for redundancy; both carry identical data but one may lag. This
// class consumes packets from either source, deduplicates by per-stream
// sequence number, detects gaps (flagging the stream for retransmission), and
// emits decoded MarketEvents in order.
//
// Socket I/O lives in the MarketDataGateway (Milestone 7); this class is the
// pure, testable decode + dedup + gap-recovery core.
#ifndef HFT_FEED_HANDLER_FEED_HANDLER_HPP_
#define HFT_FEED_HANDLER_FEED_HANDLER_HPP_

#include <cstddef>
#include <cstdint>
#include <vector>

#include "common/compiler.hpp"
#include "common/types.hpp"
#include "feed_handler/mtbt_parser.hpp"

namespace hft {

inline constexpr size_t kMaxStreams = 65536;

enum class FeedSource : uint8_t { kPrimary = 1, kSecondary = 2 };

struct StreamStats {
  uint64_t next_seq = 0;     // Next contiguous sequence number expected.
  uint64_t packets = 0;      // Packets accepted for this stream.
  uint64_t messages = 0;     // Messages emitted for this stream.
  uint64_t duplicates = 0;   // Messages already seen on the other source.
  uint64_t gaps = 0;         // Number of detected gaps.
  uint64_t last_gap_lo = 0;  // First missing seq of the most recent gap.
  uint64_t last_gap_hi = 0;  // One-past-last missing seq of that gap.
  bool needs_retransmit = false;
};

class FeedHandler {
 public:
  FeedHandler() : streams_(kMaxStreams) {}

  // Process one UDP packet from `source`. Decodes, deduplicates against the
  // other source by sequence number, and invokes emit(const MarketEvent&) for
  // each newly-seen message in order. Returns the number of messages emitted.
  template <typename EmitFn>
  size_t on_packet(FeedSource source, const uint8_t* data, size_t len, EmitFn&& emit) {
    (void)source;  // Dedup is by sequence; either source may deliver first.
    MtbtHeader hdr;
    if (!parser_.parse_header(data, len, &hdr)) {
      ++malformed_;
      return 0;
    }
    if (hdr.stream_id >= kMaxStreams) {
      ++malformed_;
      return 0;
    }
    StreamStats& st = streams_[hdr.stream_id];

    // Whole packet already consumed from the other source?
    const uint64_t packet_hi = hdr.seq_no + hdr.msg_count;
    if (st.next_seq != 0 && packet_hi <= st.next_seq) {
      st.duplicates += hdr.msg_count;
      return 0;
    }
    ++st.packets;

    size_t off = kMtbtHeaderSize;
    size_t emitted = 0;
    for (uint8_t i = 0; i < hdr.msg_count; ++i) {
      const uint64_t seq = hdr.seq_no + i;
      MarketEvent ev;
      const size_t consumed = parser_.parse_message(data + off, len - off, hdr.stream_id, seq,
                                                     hdr.timestamp, &ev);
      if (consumed == 0) {
        ++malformed_;
        break;
      }
      off += consumed;

      if (st.next_seq != 0 && seq < st.next_seq) {
        ++st.duplicates;  // Already delivered by the other source.
        continue;
      }
      // Gap: this sequence is ahead of what we expected.
      if (st.next_seq != 0 && seq > st.next_seq) {
        ++st.gaps;
        st.last_gap_lo = st.next_seq;
        st.last_gap_hi = seq;
        st.needs_retransmit = true;
      }
      st.next_seq = seq + 1;

      if (static_cast<MtbtMsgType>(ev.type) != MtbtMsgType::kHeartbeat) {
        emit(ev);
        ++emitted;
        ++st.messages;
      }
    }
    return emitted;
  }

  // Clear the retransmit flag once the gap has been requested/recovered.
  void clear_retransmit(uint16_t stream_id) {
    if (stream_id < kMaxStreams) {
      streams_[stream_id].needs_retransmit = false;
    }
  }

  const StreamStats& stream(uint16_t stream_id) const { return streams_[stream_id]; }
  uint64_t malformed() const { return malformed_; }

 private:
  MtbtParser parser_;
  std::vector<StreamStats> streams_;  // Indexed by stream id; pre-allocated.
  uint64_t malformed_ = 0;
};

}  // namespace hft

#endif  // HFT_FEED_HANDLER_FEED_HANDLER_HPP_
