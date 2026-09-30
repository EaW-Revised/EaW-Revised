namespace eawr::sim {

double source_value();

auto forbidden_auto_from_call() {
    const auto value = source_value();
    return value;
}

} // namespace eawr::sim
