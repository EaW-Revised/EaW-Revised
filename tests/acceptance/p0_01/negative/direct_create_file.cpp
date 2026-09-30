namespace eawr::sim {

extern "C" void* CreateFileW(const wchar_t* path, unsigned long access, unsigned long share,
                              void* security, unsigned long creation, unsigned long flags,
                              void* template_file);

void* forbidden_create_file() {
    return CreateFileW(L"state.bin", 0, 0, nullptr, 0, 0, nullptr);
}

} // namespace eawr::sim
