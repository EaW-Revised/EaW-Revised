// Probe for cmake/EawrMsvcGuard.cmake: exits 0 when `n == -1 ? 0 : (1 << n)` keeps its guard.
// MSVC 19.51 at /O2 drops the guard (n == -1 gives INT_MIN). Both shapes are checked: the one in
// upstream Lua's ltable.c computesizes() and the plain function. The values come from argc and a volatile so
// the compiler cannot fold them at compile time. Lua 5.0.2 is the only user, and it is why this exists.
#include <cstdio>

#define twoto(x) (1 << (x))

namespace {

int guarded(int n) {
    return (n == -1) ? 0 : twoto(n);
}

// ltable.c computesizes(), verbatim apart from the lua_assert lines.
void computesizes(int nums[], int ntotal, int* narray, int* nhash) {
    int i;
    int a = nums[0];
    int na = a;
    int n = (na == 0) ? -1 : 0;
    for (i = 1; a < *narray && *narray >= twoto(i - 1); i++) {
        if (nums[i] > 0) {
            a += nums[i];
            if (a >= twoto(i - 1)) {
                n = i;
                na = a;
            }
        }
    }
    *nhash = ntotal - na;
    *narray = (n == -1) ? 0 : twoto(n);
}

}  // namespace

int main(int argc, char**) {
    volatile int minus_one = argc - 2;  // -1 when run without arguments
    int nums[25] = {0};
    int narray = 3 + argc;  // 4: positive, so the loop is entered, and no element is counted
    int nhash = 0;
    computesizes(nums, 5, &narray, &nhash);
    const int plain = guarded(minus_one);
    if (narray != 0 || plain != 0) {
        std::printf("shift guard dropped: computesizes narray=%d (want 0), guarded(-1)=%d (want 0)\n", narray, plain);
        return 1;
    }
    return 0;
}
