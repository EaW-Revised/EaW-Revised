namespace eawr::sim {

extern "C" long read(int descriptor, void* buffer, unsigned long count);
extern "C" long write(int descriptor, const void* buffer, unsigned long count);

long forbidden_read_write(void* buffer) {
    const auto read_count = read(3, buffer, 1);
    return write(3, buffer, static_cast<unsigned long>(read_count));
}

} // namespace eawr::sim
