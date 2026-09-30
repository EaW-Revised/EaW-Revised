namespace eawr::sim {

extern "C" int close(int descriptor);

int forbidden_close() {
    return close(3);
}

} // namespace eawr::sim
