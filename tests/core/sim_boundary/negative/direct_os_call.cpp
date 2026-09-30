namespace eawr::sim {
extern "C" int open(const char* path, int flags);

int forbidden_direct_call() {
    return open("state.bin", 0);
}
} // namespace eawr::sim
