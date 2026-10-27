#pragma once
#include "eawr/mods/discovery.hpp"
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
    mods::DiscoveryInput discovery;
    mods::Catalog catalog;
    bool discovered = false;
    std::map<std::string, Page> pages;
    std::size_t sequence = 0;
    ModelState();
};
}
