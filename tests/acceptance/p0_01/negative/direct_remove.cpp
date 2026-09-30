namespace eawr::sim {

extern "C" int remove(const char* path);

int forbidden_remove() {
    return remove("state.bin");
}

} // namespace eawr::sim
