#include "../../../apps/viewer/src/galactic/report_privacy.hpp"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <string>
#include <vector>
using eawr::presentation::godot_backend::report_privacy::sanitize;
namespace eawr_renderer_test {
void report_privacy_contracts() {
    const std::vector<std::string> paths = {R"(C:\Users\ExampleUser)", R"(C:\Users\EXAMPL~1)", R"(\\server\profiles\ExampleUser)", R"(D:\redirected\AppData)", "%USERPROFILE%", "%APPDATA%", "%TEMP%"};
    const std::vector<std::string> names = {"ExampleUser", "EXAMPL~1"};
    const std::string input = R"json({"name":"ExampleUser","path":"c:\\users\\EXAMPL~1\\Save\\slot.sav","mixed":"C:/UsErS\\ExampleUser/Mods/MyExampleUserMod.xml","unc":"\\\\server\\profiles\\ExampleUser\\saves\\one.sav","redirect":"d:/redirected/AppData/log.txt","env":"%TEMP%/a %userprofile%/b","mod":"MyExampleUserMod"})json";
    const std::string expected = R"json({"name":"<user>","path":"<profile>\\Save\\slot.sav","mixed":"<profile>/Mods/MyExampleUserMod.xml","unc":"<profile>\\saves\\one.sav","redirect":"<profile>/log.txt","env":"<profile>/a <profile>/b","mod":"MyExampleUserMod"})json";
    assert(sanitize(input, paths, names) == expected);
    assert(sanitize(R"json({"n":"line\nnext and \u1234"})json", {}, {"n", "1234"}) == R"json({"n":"line\nnext and \u1234"})json");
    assert(sanitize(R"json("a\nExampleUser")json", {}, names) == R"json("a\n<user>")json");
    assert(sanitize(R"json("\tExampleUser")json", {}, names) == R"json("\t<user>")json");
    assert(sanitize(R"json("\u0020ExampleUser")json", {}, names) == R"json("\u0020<user>")json");
}
} // namespace eawr_renderer_test
