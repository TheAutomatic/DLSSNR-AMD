// The recording-ticket rule, without a device: the sequences that decide when a recording's resources may go.
#include "nr_pe_d3d12split.hpp"

#include <cstdio>
#include <vector>

int main() {
    using nr::pe::split::tickets_complete;
    struct Case { const char* name; bool sealed, discarded; std::vector<bool> receipts; bool want; };
    const std::vector<Case> cases = {
        // A recording that has never been executed is not finished, however it ends up.
        {"live, never executed", false, false, {}, false},
        // Reset succeeded with no execution: nothing ran, nothing to wait for.
        {"sealed, never executed", true, false, {}, true},
        // Discarded (Reset or invalidate) without ever running: finished.
        {"discarded, never executed", false, true, {}, true},
        // Executed once, still on the GPU, and the list then reset successfully: keep waiting for that execution.
        {"sealed, receipt pending", true, false, {false}, false},
        {"sealed, receipt finished", true, false, {true}, true},
        // Repeated Execute: every receipt must finish.
        {"sealed, two receipts, one pending", true, false, {true, false}, false},
        {"sealed, two receipts, both finished", true, false, {true, true}, true},
        // Discarded after execution: the already-submitted work still holds resources.
        {"discarded, receipt pending", false, true, {false}, false},
        {"discarded, receipt finished", false, true, {true}, true},
        // A failed signal leaves an unknown receipt that is never done.
        {"sealed, unknown receipt", true, false, {false}, false},
    };
    int bad = 0;
    for (const auto& c : cases) {
        const bool got = tickets_complete(c.sealed, c.discarded, c.receipts);
        if (got != c.want) {
            std::printf("FAIL %s: got %d want %d\n", c.name, got ? 1 : 0, c.want ? 1 : 0);
            ++bad;
        } else {
            std::printf("ok   %s\n", c.name);
        }
    }
    std::printf("%d failure(s)\n", bad);
    return bad ? 1 : 0;
}
