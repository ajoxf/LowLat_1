// Metrics emitter for the monitoring / research plane.
//
// Writes a JSON-lines (.jsonl) stream -- one snapshot object per line -- that
// the HTML dashboard renders. This runs OFF the hot path: the backtester calls
// it between events, and in production the monitoring thread (Core 7) would call
// it once a second from a shared-memory copy of the counters. It never touches
// the trading hot path.
#ifndef HFT_MONITOR_METRICS_HPP_
#define HFT_MONITOR_METRICS_HPP_

#include <cstdint>
#include <cstdio>

namespace hft {

// One point on the dashboard's time axis.
struct MetricSnapshot {
  uint64_t t = 0;               // X-axis: event index (or epoch ms when live).
  uint64_t events = 0;         // Cumulative market events processed.
  uint64_t orders_sent = 0;    // Cumulative orders sent.
  uint64_t orders_rejected = 0;// Cumulative risk rejections.
  uint64_t fills = 0;          // Cumulative fills.
  int64_t position = 0;        // Current net position (units).
  int64_t pnl_inr = 0;         // Cumulative mark-to-market PnL (rupees).
  uint64_t lat_p50 = 0;        // Tick-to-trade percentiles (ns).
  uint64_t lat_p99 = 0;
  uint64_t lat_p999 = 0;
};

class MetricsWriter {
 public:
  MetricsWriter() = default;
  ~MetricsWriter() { close(); }

  MetricsWriter(const MetricsWriter&) = delete;
  MetricsWriter& operator=(const MetricsWriter&) = delete;

  bool open(const char* path) {
    close();
    file_ = std::fopen(path, "w");
    return file_ != nullptr;
  }

  // Append one snapshot as a JSON object on its own line.
  void write(const MetricSnapshot& s) {
    if (file_ == nullptr) {
      return;
    }
    std::fprintf(file_,
                 "{\"t\":%llu,\"events\":%llu,\"orders_sent\":%llu,"
                 "\"orders_rejected\":%llu,\"fills\":%llu,\"position\":%lld,"
                 "\"pnl_inr\":%lld,\"lat_p50\":%llu,\"lat_p99\":%llu,"
                 "\"lat_p999\":%llu}\n",
                 static_cast<unsigned long long>(s.t),
                 static_cast<unsigned long long>(s.events),
                 static_cast<unsigned long long>(s.orders_sent),
                 static_cast<unsigned long long>(s.orders_rejected),
                 static_cast<unsigned long long>(s.fills),
                 static_cast<long long>(s.position), static_cast<long long>(s.pnl_inr),
                 static_cast<unsigned long long>(s.lat_p50),
                 static_cast<unsigned long long>(s.lat_p99),
                 static_cast<unsigned long long>(s.lat_p999));
    ++rows_;
  }

  void flush() {
    if (file_ != nullptr) {
      std::fflush(file_);
    }
  }

  void close() {
    if (file_ != nullptr) {
      std::fclose(file_);
      file_ = nullptr;
    }
  }

  uint64_t rows() const { return rows_; }

 private:
  std::FILE* file_ = nullptr;
  uint64_t rows_ = 0;
};

}  // namespace hft

#endif  // HFT_MONITOR_METRICS_HPP_
