#include "../src/core/nr_diag_build.hpp"
#include <cassert>
#include <string>
#include <vector>

static std::vector<std::string> lines;
static void capture(const char* line) { lines.emplace_back(line); }

int main() {
    using namespace nr::diagbuild;
    for (const char* value : {static_cast<const char*>(nullptr), "", "baseline", "LATE", "late ", "invalid"})
        assert(parse(value) == Mode::none);
    assert(parse("late") == Mode::late);
    assert(parse("late-gpu") == Mode::late_gpu);
    assert(parse("late-pipelines") == Mode::late_pipelines);
    assert(std::string(name(Mode::none)) == "baseline");
    for (const char* value : {static_cast<const char*>(nullptr), "", "-1", "nan", "inf", "1x", " 2", "2 ", "1e999", "86401"})
        assert(delay(value) == 8.0);
    assert(delay("0") == 0.0);
    assert(delay("2.5") == 2.5);
    assert(delay("86400") == 86400.0);
    const auto start = origin();
    assert(origin() == start);
    assert(std::chrono::duration<double>(deadline(8) - start).count() == 8.0);
    nr::set_log_sink(capture);
    setenv("NR_DIAG_BUILD", "late-gpu", 1);
    setenv("NR_DIAG_BUILD_DELAY", "0.04", 1);
    wait(Mode::late_gpu, "render-thread-guard");
    assert(lines.empty());
    {
        Background scope;
        assert(selected(Mode::late_gpu));
        wait(Mode::late, "wrong-mode");
        assert(lines.empty());
        std::thread render([] { assert(!background); wait(Mode::late_gpu, "other-thread"); });
        render.join();
        assert(lines.empty());
        wait(Mode::late_gpu, "test-gpu");
        assert(Clock::now() >= deadline(0.04));
        assert(lines.size() == 2);
        assert(lines[0].find("wait begin test-gpu: t=") != std::string::npos);
        assert(lines[1].find("wait end test-gpu: t=") != std::string::npos);
        setenv("NR_DIAG_BUILD_DELAY", "0", 1);
        wait(Mode::late_gpu, "expired");
        assert(lines.size() == 4);
        unsetenv("NR_DIAG_BUILD");
        wait(Mode::late_gpu, "baseline");
        assert(lines.size() == 4);
        setenv("NR_DIAG_BUILD", "invalid", 1);
        wait(Mode::late_gpu, "invalid");
        assert(lines.size() == 4);
    }
    assert(!background);
    nr::set_log_sink(nullptr);
    std::puts("PASS: build parser valid/unset/empty/invalid; delay default/override/zero/fraction/range/error; load-relative deadline; thread-local render guard; matching-mode begin/end; expired deadline; baseline isolation");
}
