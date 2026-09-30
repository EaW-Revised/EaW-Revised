namespace eawr::sim {

template <typename T>
T forbidden_template(T value) {
    return value;
}

float instantiate_forbidden_template() {
    return forbidden_template<float>(1.25F);
}

} // namespace eawr::sim
