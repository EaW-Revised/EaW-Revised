#pragma once
#include "eawr/mods/content_digest.hpp"
#include "eawr/mods/discovery.hpp"
#include <memory>
#include <map>
#include <array>
namespace eawr::mods_service {
struct Page {
    std::string method;
    std::array<std::uint8_t, 32> digest{};
    std::size_t offset{};
};
struct ModelState {
    mods::steam::fs::path user_data;
    mods::steam::fs::path game_root;
    mods::steam::fs::path player_dir;
    mods::DiscoveryInput discovery;
    mods::Catalog catalog;
    bool discovered = false;
    std::map<std::string, Page> pages;
    std::size_t sequence = 0;
    // Content digests (protocol 1.14): created on first use under <user data>/mods/cache/content.
    std::shared_ptr<mods::ContentDigests> digests;
    ModelState();
};
}
