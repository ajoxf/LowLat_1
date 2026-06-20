// Compiler / branch-prediction helper macros used on the hot path.
#ifndef HFT_COMMON_COMPILER_HPP_
#define HFT_COMMON_COMPILER_HPP_

// Force-inline annotation for hot-path functions.
#if defined(__GNUC__) || defined(__clang__)
#define HFT_ALWAYS_INLINE [[gnu::always_inline]] inline
#define HFT_NOINLINE [[gnu::noinline]]
#define HFT_LIKELY(x) __builtin_expect(!!(x), 1)
#define HFT_UNLIKELY(x) __builtin_expect(!!(x), 0)
#define HFT_PREFETCH(addr) __builtin_prefetch(addr)
#define HFT_RESTRICT __restrict__
#else
#define HFT_ALWAYS_INLINE inline
#define HFT_NOINLINE
#define HFT_LIKELY(x) (x)
#define HFT_UNLIKELY(x) (x)
#define HFT_PREFETCH(addr)
#define HFT_RESTRICT
#endif

// Size of a cache line on the target hardware (Xeon / Sapphire Rapids: 64 B).
namespace hft {
inline constexpr unsigned long kCacheLineSize = 64;
}  // namespace hft

#endif  // HFT_COMMON_COMPILER_HPP_
