#pragma once
#include <array>
#include <cstdint>

// Invented shader-model bytecode: transform a position, forward UV and colour,
// sample one texture and multiply by interpolated colour. No game-derived bytes.
namespace shader_bridge_gate {
inline constexpr std::array<std::uint32_t, 21> vertex{
    0xfffe0101,
    0x1f, 0x80000000, 0x900f0000,
    0x1f, 0x80000005, 0x900f0001,
    0x1f, 0x8000000a, 0x900f0002,
    0x14, 0xc00f0000, 0x90e40000, 0xa0e40000,
    0x01, 0xe0030000, 0x90e40001,
    0x01, 0xd00f0000, 0x90e40002, 0x0000ffff};
inline constexpr std::array<std::uint32_t, 22> pixel2{
    0xffff0200,
    0x0200001f, 0x80000000, 0xb0030000,
    0x0200001f, 0x80000000, 0x900f0000,
    0x0200001f, 0x90000000, 0xa00f0800,
    0x03000042, 0x800f0000, 0xb0e40000, 0xa0e40800,
    0x03000005, 0x800f0000, 0x80e40000, 0x90e40000,
    0x02000001, 0x800f0800, 0x80e40000, 0x0000ffff};
inline constexpr std::array<std::uint32_t, 8> pixel1{
    0xffff0101, 0x42, 0xb00f0000,
    0x05, 0x800f0000, 0xb0e40000, 0x90e40000, 0x0000ffff};
}
