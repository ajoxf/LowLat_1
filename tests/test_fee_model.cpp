// Tests for the NSE transaction-cost / rebate model.
#include "sim/fee_model.hpp"

#include <gtest/gtest.h>

#include "common/types.hpp"

namespace hft {
namespace {

// A round number makes the arithmetic checkable: 1 bp of a Rs 10,000 notional
// (1,000,000 paise) is Rs 1 = 100 paise. bp100 = 100 means exactly 1 bp.
TEST(FeeModel, OneBasisPointArithmetic) {
  FeeModel f;
  f.txn_bp100 = 100;  // 1 bp
  f.sebi_bp100 = 0;
  f.stt_sell_bp100 = 0;
  f.stamp_buy_bp100 = 0;
  f.gst_pct = 0;
  f.maker_rebate_bp100 = 0;
  // notional = 1,000,000 paise, buy side.
  EXPECT_EQ(f.cost_paise(kBuy, 100000, 10), 100);  // 100000 paise * 10 = 1e6 paise
}

// STT is charged only on the sell leg; stamp duty only on the buy leg.
TEST(FeeModel, AsymmetricStatutoryCharges) {
  FeeModel f;
  f.txn_bp100 = 0;
  f.sebi_bp100 = 0;
  f.gst_pct = 0;
  f.stt_sell_bp100 = 200;   // 2 bp on sells
  f.stamp_buy_bp100 = 20;   // 0.2 bp on buys
  f.maker_rebate_bp100 = 0;
  // notional 1e6 paise.
  const int64_t buy = f.cost_paise(kBuy, 100000, 10);
  const int64_t sell = f.cost_paise(kSell, 100000, 10);
  EXPECT_EQ(buy, 20);    // stamp only: 0.2 bp of Rs 10,000 = Rs 0.20 = 20 paise
  EXPECT_EQ(sell, 200);  // STT only: 2 bp = Rs 2 = 200 paise
}

// A large enough maker rebate turns a cost into a net credit (negative cost).
TEST(FeeModel, RebateCanFlipToCredit) {
  FeeModel f;
  f.txn_bp100 = 19;
  f.sebi_bp100 = 1;
  f.stt_sell_bp100 = 0;   // isolate the maker vs charge comparison on a buy
  f.stamp_buy_bp100 = 0;
  f.gst_pct = 0;
  f.maker_rebate_bp100 = 0;
  const int64_t cost = f.cost_paise(kBuy, 100000, 10);
  EXPECT_GT(cost, 0);  // net cost without a rebate

  f.maker_rebate_bp100 = 100;  // 1 bp rebate > the ~0.2 bp of charges
  const int64_t credited = f.cost_paise(kBuy, 100000, 10);
  EXPECT_LT(credited, 0);  // now a net credit
}

// GST is applied on (txn + sebi), not on STT/stamp.
TEST(FeeModel, GstOnChargesOnly) {
  FeeModel f;
  f.txn_bp100 = 100;   // 1 bp -> 100 paise on 1e6 notional
  f.sebi_bp100 = 0;
  f.stt_sell_bp100 = 0;
  f.stamp_buy_bp100 = 0;
  f.gst_pct = 18;
  f.maker_rebate_bp100 = 0;
  // 100 paise txn + 18% GST = 118 paise.
  EXPECT_EQ(f.cost_paise(kBuy, 100000, 10), 118);
}

}  // namespace
}  // namespace hft
