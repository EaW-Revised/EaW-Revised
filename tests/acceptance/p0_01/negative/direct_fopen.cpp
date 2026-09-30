namespace eawr::sim {

extern "C" void* fopen(const char* path, const char* mode);

void* forbidden_fopen() {
    return fopen("state.bin", "rb");
}

} // namespace eawr::sim
