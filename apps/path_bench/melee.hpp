#pragma once

namespace eawr::bench {

// path_bench --melee (#601): the close-range battle benchmark; melee.cpp describes it.
[[nodiscard]] int melee_main(int argc, const char* const argv[]);
// path_bench --profile-attach <pid> (#601): samples another process, the viewer (sampler.hpp).
[[nodiscard]] int attach_main(int argc, const char* const argv[]);
// path_bench --land: the land path search on a stock land map (land_bench.cpp).
[[nodiscard]] int land_main(int argc, const char* const argv[]);

} // namespace eawr::bench
