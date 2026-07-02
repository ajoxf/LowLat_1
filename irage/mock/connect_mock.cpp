// Implementation of the mock platform statics and stub launch functions.
// Only compiled in this repo (local build/test); never on the iRage box.
#include "ConnectHeader.h"

std::map<std::string, std::unique_ptr<alpha::AlphaStrategy>> ExternalBaseImpl::stratMap;

void initAndRun(int /*argc*/, char** /*argv*/) {
  // Real platform: parses configs, spawns SEND/RECV/UPDT/GUID threads, runs
  // the event loop. The mock is driven directly by unit tests instead.
}

namespace alpha {
void DoPlatformInit(int /*argc*/, char** /*argv*/) {}
void GetPortfolios(std::vector<std::map<int, std::string>>& configMaps,
                   std::map<int, std::string>& configMap) {
  configMaps.push_back(configMap);
}
void DoPlatformRun(std::vector<std::map<int, std::string>>& /*configMaps*/) {}
}  // namespace alpha
