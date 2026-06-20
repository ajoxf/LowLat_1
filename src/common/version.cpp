// Anchors the hft_core static library with at least one translation unit and
// exposes a build version string.
namespace hft {
const char* version() { return "nse-hft 0.1.0"; }
}  // namespace hft
