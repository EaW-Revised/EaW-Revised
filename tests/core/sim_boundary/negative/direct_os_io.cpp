#include <fstream>

namespace eawr::sim {
void forbidden_file_access() {
    std::ifstream input("state.bin");
}
} // namespace eawr::sim
