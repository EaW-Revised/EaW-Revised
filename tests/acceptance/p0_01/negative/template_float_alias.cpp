namespace eawr::sim {

template <typename T>
using Scalar = T;

template <typename T>
struct Holder {
    T value;
};

template struct Holder<double>;

Scalar<float> forbidden_template_alias() {
    return 0.5F;
}

} // namespace eawr::sim
