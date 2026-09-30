namespace eawr::sim {
using FixedStorage = long long;

FixedStorage multiply_fixed(const FixedStorage left, const FixedStorage right) {
    return (left * right) >> 16;
}
} // namespace eawr::sim
