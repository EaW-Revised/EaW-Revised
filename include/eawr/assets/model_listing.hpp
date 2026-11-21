#pragma once

// Every winning model file of a mounted view, in the view's order. It lives with the asset-name resolver because it
// owns the model folder and extension (a listing is not the lookup of an authored name, so it is not a resolver call).

#include "eawr/core/result.hpp"
#include "eawr/vfs/vfs.hpp"

#include <vector>

namespace eawr::assets {

[[nodiscard]] core::Result<std::vector<vfs::AssetRecord>> list_models(const vfs::Vfs& filesystem);

} // namespace eawr::assets
