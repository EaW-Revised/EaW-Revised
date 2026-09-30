namespace eawr::core::detail {

#if defined(EAWR_INJECT_PROJECT_WARNING)
[[deprecated("EAWR deliberate project warning probe")]] static void warning_probe_target() {}

void trigger_warning_probe() {
    warning_probe_target();
}
#else
void warning_probe_disabled() {}
#endif

} // namespace eawr::core::detail
