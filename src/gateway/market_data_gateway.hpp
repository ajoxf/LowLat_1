// MTBT market-data gateway: receives NSE multicast on both Source 1 and
// Source 2 for each stream, hands packets to the FeedHandler for dedup + gap
// detection, and emits decoded MarketEvents into a ring buffer for the order
// book thread. Runs on a dedicated pinned core in a tight busy-poll loop.
#ifndef HFT_GATEWAY_MARKET_DATA_GATEWAY_HPP_
#define HFT_GATEWAY_MARKET_DATA_GATEWAY_HPP_

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "common/compiler.hpp"
#include "common/ring_buffer.hpp"
#include "common/types.hpp"
#include "feed_handler/feed_handler.hpp"
#include "gateway/udp_socket.hpp"

namespace hft {

// One MTBT stream: two redundant multicast sources plus a port.
struct StreamConfig {
  std::string source1_group;  // Source 1 multicast IP.
  std::string source2_group;  // Source 2 multicast IP.
  uint16_t port = 0;
  uint16_t stream_id = 0;
};

// Ring buffer carrying decoded events to the order-book thread.
using EventQueue = RingBuffer<MarketEvent, 1U << 16>;

class MarketDataGateway {
 public:
  MarketDataGateway(FeedHandler* feed, EventQueue* out, std::string iface_ip)
      : feed_(feed), out_(out), iface_ip_(std::move(iface_ip)), running_(false) {}

  // Subscribe to a stream's two sources. Returns false if a join fails.
  bool subscribe(const StreamConfig& cfg) {
    Subscription sub;
    sub.cfg = cfg;
    if (!sub.s1.bind_rx(cfg.port) || !sub.s1.join_multicast(cfg.source1_group.c_str(),
                                                            iface_ip_.c_str())) {
      return false;
    }
    if (!sub.s2.bind_rx(cfg.port) || !sub.s2.join_multicast(cfg.source2_group.c_str(),
                                                            iface_ip_.c_str())) {
      return false;
    }
    sub.s1.set_nonblocking(true);
    sub.s2.set_nonblocking(true);
    subs_.push_back(std::move(sub));
    return true;
  }

  // One non-blocking poll pass over every subscription/source. Returns the
  // number of MarketEvents pushed to the output queue. Call this in the
  // gateway thread's spin loop.
  size_t poll_once() {
    size_t pushed = 0;
    for (Subscription& sub : subs_) {
      pushed += DrainSource(sub.s1, FeedSource::kPrimary);
      pushed += DrainSource(sub.s2, FeedSource::kSecondary);
    }
    return pushed;
  }

  // Dedicated-thread spin loop. Stop with stop().
  void run() {
    running_.store(true, std::memory_order_release);
    while (running_.load(std::memory_order_acquire)) {
      poll_once();
    }
  }
  void stop() { running_.store(false, std::memory_order_release); }

  uint64_t dropped() const { return dropped_; }

 private:
  static constexpr size_t kBatch = 32;
  static constexpr size_t kPktCap = 2048;

  struct Subscription {
    StreamConfig cfg;
    UdpSocket s1;
    UdpSocket s2;
  };

  size_t DrainSource(UdpSocket& sock, FeedSource source) {
    if (!sock.valid()) {
      return 0;
    }
    uint32_t lens[kBatch];
    int n = sock.recv_batch(rx_, kPktCap, kBatch, lens);
    if (n <= 0) {
      return 0;
    }
    size_t pushed = 0;
    for (int i = 0; i < n; ++i) {
      pushed += feed_->on_packet(source, rx_ + i * kPktCap, lens[i],
                                 [&](const MarketEvent& ev) {
                                   if (HFT_UNLIKELY(!out_->push(ev))) {
                                     ++dropped_;
                                   }
                                 });
    }
    return pushed;
  }

  FeedHandler* feed_;
  EventQueue* out_;
  std::string iface_ip_;
  std::vector<Subscription> subs_;
  std::atomic<bool> running_;
  uint64_t dropped_ = 0;
  uint8_t rx_[kBatch * kPktCap];
};

}  // namespace hft

#endif  // HFT_GATEWAY_MARKET_DATA_GATEWAY_HPP_
