#include "eawr/platform/native_read.hpp"

#include <fstream>

namespace eawr::platform {

NativeReadResult read_native_chunks(const std::filesystem::path& file, const std::span<std::byte> buffer,
    const std::function<bool(std::span<const std::byte>)>& consume) {
    NativeReadResult result;
    if (buffer.empty() || !consume) return result;
    std::ifstream input(file, std::ios::binary);
    if (!input) return result;
    input.rdbuf()->pubsetbuf(nullptr, 0);   // the caller reads in its own chunks
    while (true) {
        input.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(buffer.size()));
        const auto got = static_cast<std::size_t>(input.gcount());
        if (got > 0) {
            result.bytes += got;
            if (!consume(buffer.first(got))) {
                result.ok = true;
                return result;
            }
        }
        if (input.eof()) {
            result.ok = true;
            return result;
        }
        if (!input) return result;
    }
}

} // namespace eawr::platform
