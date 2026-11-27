#pragma once
#include "nr_renodx_keys.hpp"
namespace nr::wilsjo2_keys {
// PostHistory (ours, the network's post-block history blend) - in Network on a layer page, in the ReShade route's own group
inline constexpr renodx_keys::Key kPostHistory = {
    "PostHistory", renodx_keys::kFloat, 2, 0.0f, 1.0f, 1.0f, "Network", "Post history",
    "PostHistory (ours). How much the network's output block blends its previous answer, moved along the motion vectors, into this one: 1 = as NVIDIA's DLL (default), 0 = off. Without motion vectors there is no history either way (diagnostic).", nullptr};
inline constexpr renodx_keys::Key kPostHistoryOwn = {
    "PostHistory", renodx_keys::kFloat, 2, 0.0f, 1.0f, 1.0f, "This route's own", "Post history",
    "PostHistory (ours). How much the network's output block blends its previous answer, moved along the motion vectors, into this one: 1 = as NVIDIA's DLL (default), 0 = off. Without motion vectors there is no history either way (diagnostic).", nullptr};
}
