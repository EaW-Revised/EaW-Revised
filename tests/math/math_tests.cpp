#include "math_support.hpp"
#include "eawr/sim/math/math.hpp"
#include "../../src/sim/math/cordic_constants.hpp"
#include "../../src/sim/math/wide.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <limits>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace math_test_support {


int failures = 0;

void expect(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

void expect_raw(const eawr::core::Result<Fixed>& result, std::int64_t raw,
                std::string_view message) {
    expect(result.has_value(), message);
    if (result) expect(result.value().raw() == raw, message);
}

std::uint64_t distance(std::int64_t left, std::int64_t right) {
    return left >= right ? static_cast<std::uint64_t>(left - right)
                         : static_cast<std::uint64_t>(right - left);
}

void scalar_tests() {
    expect_raw(Fixed::from_integer(-(std::int64_t{1} << 39)),
               std::numeric_limits<std::int64_t>::min(), "minimum integer construction");
    expect(!Fixed::from_integer(std::int64_t{1} << 39), "positive integer overflow");
    expect_raw(Fixed::from_ratio(1, 2 * Fixed::scale), 0, "positive half to even zero");
    expect_raw(Fixed::from_ratio(3, 2 * Fixed::scale), 2, "positive 1.5 to even two");
    expect_raw(Fixed::from_ratio(-3, 2 * Fixed::scale), -2, "negative 1.5 symmetric");
    expect_raw(Fixed::from_decimal("0.0000000298023223876953125"), 0,
               "decimal half quantum to zero");
    expect_raw(Fixed::from_decimal("-0.0000000894069671630859375"), -2,
               "decimal negative 1.5 quantum");
    expect_raw(Fixed::from_decimal("+1.5e1"), 15 * Fixed::scale,
               "decimal exponent exact");
    expect_raw(Fixed::from_decimal("549755813887.999999940395355224609375"),
               std::numeric_limits<std::int64_t>::max(), "maximum exact decimal");
    expect_raw(Fixed::from_decimal("-549755813888"),
               std::numeric_limits<std::int64_t>::min(), "minimum exact decimal");
    expect(!Fixed::from_decimal("549755813887.9999999701976776123046875"),
           "positive boundary tie rounds out of range");
    expect(!Fixed::from_decimal(" 1"), "decimal whitespace rejected");
    expect(!Fixed::from_decimal("1e999999999"), "huge exponent overflows without allocation");
    expect_raw(Fixed::from_decimal("1e-999999999"), 0,
               "huge negative exponent underflows without allocation");
    expect(!Fixed::from_decimal(std::string(4097, '1')), "decimal resource limit");

    expect(!add(Fixed::from_raw(std::numeric_limits<std::int64_t>::max()),
                Fixed::from_raw(1)), "add overflow");
    expect(!subtract(Fixed::from_raw(std::numeric_limits<std::int64_t>::min()),
                     Fixed::from_raw(1)), "subtract overflow");
    expect(!negate(Fixed::from_raw(std::numeric_limits<std::int64_t>::min())),
           "negate minimum overflow");
    expect_raw(multiply(Fixed::from_raw(1), Fixed::from_raw(Fixed::scale / 2)), 0,
               "multiply half to even");
    expect_raw(multiply(Fixed::from_raw(3), Fixed::from_raw(Fixed::scale / 2)), 2,
               "multiply 1.5 to even");
    expect_raw(multiply(Fixed::from_raw(-3), Fixed::from_raw(Fixed::scale / 2)), -2,
               "multiply negative 1.5");
    expect_raw(multiply(Fixed::from_raw(std::numeric_limits<std::int64_t>::min()),
                        Fixed::from_raw(Fixed::scale)),
               std::numeric_limits<std::int64_t>::min(), "multiply minimum by one");
    expect(!multiply(Fixed::from_raw(std::numeric_limits<std::int64_t>::min()),
                     Fixed::from_raw(-Fixed::scale)), "multiply minimum by negative one");
    expect(!divide(Fixed::from_raw(1), Fixed{}), "divide by zero");
    expect_raw(divide(Fixed::from_raw(std::numeric_limits<std::int64_t>::min()),
                      Fixed::from_raw(Fixed::scale)),
               std::numeric_limits<std::int64_t>::min(), "divide minimum by one");
    expect(!divide(Fixed::from_raw(std::numeric_limits<std::int64_t>::min()),
                   Fixed::from_raw(-Fixed::scale)), "divide minimum by negative one");
    expect_raw(sqrt(Fixed::from_raw(4 * Fixed::scale)), 2 * Fixed::scale,
               "integer square root");
    expect(!sqrt(Fixed::from_raw(-1)), "negative square root");
    expect_raw(sqrt(Fixed::from_raw(2)), 5793, "irrational square root rounds nearest");

    const Fixed negative = Fixed::from_raw(-Fixed::scale - Fixed::scale / 2);
    expect(negative.trunc_to_integer() == -1, "explicit truncation");
    expect(negative.floor_to_integer() == -2, "explicit floor");
    expect(negative.ceil_to_integer() == -1, "explicit ceiling");
    expect(negative.nearest_even_to_integer() == -2, "explicit nearest-even integer");
}

void geometry_tests() {
    auto unit_x = Fixed::from_integer(1).value();
    auto unit_y = Fixed::from_integer(1).value();
    auto zero = Fixed{};
    auto crossed = cross({unit_x,zero,zero}, {zero,unit_y,zero});
    expect(crossed && crossed.value() == Vec3{zero,zero,unit_x}, "right-handed cross");

    auto normalized = normalize(Vec3{
        Fixed::from_raw(std::numeric_limits<std::int64_t>::min()),
        Fixed::from_raw(1), Fixed::from_raw(std::numeric_limits<std::int64_t>::max())});
    expect(normalized.has_value(), "full-domain nonzero vector normalizes");
    expect(!normalize(Vec3{}), "zero vector normalization error");
    expect_raw(length(Vec2{Fixed::from_raw(3),Fixed::from_raw(4)}), 5,
               "raw Pythagorean length");
    expect(!length(Vec3{
        Fixed::from_raw(std::numeric_limits<std::int64_t>::min()),
        Fixed::from_raw(std::numeric_limits<std::int64_t>::min()),
        Fixed::from_raw(std::numeric_limits<std::int64_t>::min())}),
        "standalone full-domain length reports overflow");

    auto cancellation = dot(
        Quat{Fixed::from_raw(std::numeric_limits<std::int64_t>::min()),
             Fixed::from_raw(std::numeric_limits<std::int64_t>::min()),
             Fixed::from_raw(std::numeric_limits<std::int64_t>::min()),
             Fixed::from_raw(std::numeric_limits<std::int64_t>::min())},
        Quat{Fixed::from_raw(std::numeric_limits<std::int64_t>::min()),
             Fixed::from_raw(std::numeric_limits<std::int64_t>::max()),
             Fixed::from_raw(std::numeric_limits<std::int64_t>::min()),
             Fixed::from_raw(std::numeric_limits<std::int64_t>::max())});
    expect_raw(cancellation, std::int64_t{1} << 40, "four-product 130-bit cancellation");

    auto two = Fixed::from_integer(2).value();
    auto three = Fixed::from_integer(3).value();
    auto four = Fixed::from_integer(4).value();
    auto matrix = identity_matrix();
    matrix.rows[0][3]=two; matrix.rows[1][3]=three; matrix.rows[2][3]=four;
    auto point = transform_point(matrix, {unit_x,zero,zero});
    auto vector = transform_vector(matrix, {unit_x,zero,zero});
    expect(point && point.value() == Vec3{three,three,four}, "affine point translation");
    expect(vector && vector.value() == Vec3{unit_x,zero,zero}, "vector excludes translation");

    const Quat half_turn{zero,zero,unit_x,zero};
    auto rotation = to_matrix(half_turn, {});
    expect(rotation.has_value(), "unit quaternion converts to matrix");
    if (rotation) {
        auto rotated = transform_vector(rotation.value(), {unit_x,zero,zero});
        expect(rotated && rotated.value() == Vec3{Fixed::from_raw(-Fixed::scale),zero,zero},
               "half-turn quaternion convention");
    }
    expect(!to_matrix(Quat{unit_x,unit_x,zero,zero}, {}), "non-unit quaternion rejected");
}

std::vector<std::string> split(std::string line) {
    std::vector<std::string> fields;
    std::stringstream stream(std::move(line));
    for (std::string field; std::getline(stream, field, ',');) fields.push_back(field);
    return fields;
}

void oracle_vectors(const std::string& path, const std::string& output_path) {
    std::ifstream input(path);
    expect(input.good(), "oracle vector file opens");
    std::ofstream output;
    if (!output_path.empty()) {
        output.open(output_path, std::ios::binary);
        expect(output.good(), "oracle output file opens");
        output << "operation,a,b,c,actual_raw\n";
    }
    std::string line;
    std::getline(input, line);
    while (std::getline(input, line)) {
        auto fields = split(line);
        expect(fields.size() == 5, "oracle row shape");
        if (fields.size() != 5) continue;
        const auto a = std::stoll(fields[1]);
        const auto b = std::stoll(fields[2]);
        const auto c = std::stoll(fields[3]);
        const auto expected = std::stoll(fields[4]);
        std::int64_t actual = 0;
        if (fields[0] == "sin") actual = sin_turn(Fixed::from_raw(a)).raw();
        else if (fields[0] == "cos") actual = cos_turn(Fixed::from_raw(a)).raw();
        else if (fields[0] == "atan2") {
            auto value = atan2_turn(Fixed::from_raw(a), Fixed::from_raw(b));
            expect(value.has_value(), "atan2 oracle input defined");
            if (value) actual = value.value().raw();
        } else if (fields[0] == "norm2x" || fields[0] == "norm2y") {
            auto value = normalize(Vec2{Fixed::from_raw(a),Fixed::from_raw(b)}).value();
            actual = fields[0].back() == 'x' ? value.x.raw() : value.y.raw();
        } else if (fields[0].rfind("norm3", 0) == 0) {
            auto value = normalize(Vec3{Fixed::from_raw(a),Fixed::from_raw(b),Fixed::from_raw(c)}).value();
            actual = fields[0].back() == 'x' ? value.x.raw()
                   : fields[0].back() == 'y' ? value.y.raw() : value.z.raw();
        } else if (fields[0] == "norm4x") {
            auto value = normalize(Quat{
                Fixed::from_raw(a),Fixed::from_raw(b),Fixed::from_raw(c),Fixed::from_raw(a)}).value();
            actual = value.x.raw();
        }
        if (distance(actual, expected) > 4) {
            std::cerr << "oracle mismatch " << fields[0] << " a=" << a << " b=" << b
                      << " c=" << c << " actual=" << actual << " expected=" << expected << '\n';
            expect(false, "high-precision oracle error budget");
        }
        if (output) {
            output << fields[0] << ',' << a << ',' << b << ',' << c << ',' << actual << '\n';
        }
    }
}

void trig_exact_tests() {
    constexpr std::int64_t quarter = Fixed::scale / 4;
    constexpr std::int64_t half = Fixed::scale / 2;
    expect(sin_turn(Fixed{}).raw() == 0, "sin zero exact");
    expect(sin_turn(Fixed::from_raw(quarter)).raw() == Fixed::scale, "sin quarter exact");
    expect(cos_turn(Fixed::from_raw(half)).raw() == -Fixed::scale, "cos half exact");
    expect(wrap_turn(Fixed::from_raw(half)).raw() == -half, "positive half wraps negative");
    expect(atan2_turn(Fixed{}, Fixed::from_raw(-1)).value().raw() == -half,
           "atan2 negative x axis exact");
    expect(!atan2_turn(Fixed{}, Fixed{}), "atan2 zero undefined");
}

} // namespace

using namespace math_test_support;

int main(int argc, char** argv) {
    scalar_tests();
    geometry_tests();
    trig_exact_tests();
    wide_fast_path_tests();
    fixed_fast_path_tests();
    wide_limb_tests();
    if (argc == 2) oracle_vectors(argv[1], {});
    else if (argc == 4 && std::string(argv[2]) == "--output") {
        oracle_vectors(argv[1], argv[3]);
    } else expect(false, "oracle vector path argument required");
    if (failures != 0) {
        std::cerr << failures << " math checks failed\n";
        return 1;
    }
    std::cout << "math contracts passed\n";
    return 0;
}
