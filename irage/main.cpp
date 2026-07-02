// Entry point for the iRage ConnectLib strategy binary ("normal way": trading
// with the blotter GUI, per API v3.4 section 3.2.1). The platform launches
// portfolios from prod/ui/portfolio.conf, each running a clone of strat_1.
#include "mystrat.h"

int main(int argc, char** argv) {
  std::unique_ptr<alpha::AlphaStrategy> strat_1_ptr(new my_strat_ns::strat_1);
  ExternalBaseImpl::stratMap.insert(std::make_pair("strat_1", std::move(strat_1_ptr)));
  initAndRun(argc, argv);
  return 0;
}
