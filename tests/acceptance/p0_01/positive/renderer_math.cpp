namespace eawr::presentation {

float renderer_lerp_supplemental(const float left, const float right) {
    return left + (right - left) * 0.5F;
}

} // namespace eawr::presentation
