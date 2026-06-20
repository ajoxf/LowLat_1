// Tests for the dashboard metrics writer.
#include "monitor/metrics.hpp"

#include <gtest/gtest.h>

#include <cstdio>
#include <fstream>
#include <string>

namespace hft {
namespace {

TEST(MetricsWriter, WritesJsonLines) {
  const char* path = "/tmp/hft_metrics_test.jsonl";
  {
    MetricsWriter w;
    ASSERT_TRUE(w.open(path));
    for (uint64_t i = 1; i <= 3; ++i) {
      MetricSnapshot s;
      s.t = i;
      s.events = i * 100;
      s.orders_sent = i * 2;
      s.orders_rejected = i;
      s.fills = i;
      s.position = -static_cast<int64_t>(i);
      s.pnl_inr = static_cast<int64_t>(i) * 10 - 25;  // crosses zero
      s.lat_p50 = 2000;
      s.lat_p99 = 5000;
      s.lat_p999 = 9000;
      w.write(s);
    }
    w.flush();
    EXPECT_EQ(w.rows(), 3u);
  }
  // Read back: exactly three non-empty JSON-object lines with expected keys.
  std::ifstream in(path);
  ASSERT_TRUE(in.is_open());
  int lines = 0;
  std::string ln;
  while (std::getline(in, ln)) {
    if (ln.empty()) continue;
    ++lines;
    EXPECT_EQ(ln.front(), '{');
    EXPECT_EQ(ln.back(), '}');
    EXPECT_NE(ln.find("\"pnl_inr\""), std::string::npos);
    EXPECT_NE(ln.find("\"lat_p99\""), std::string::npos);
  }
  EXPECT_EQ(lines, 3);
}

}  // namespace
}  // namespace hft
