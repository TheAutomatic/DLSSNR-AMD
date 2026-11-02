// The recording ticket through its real path - book, note_execution, seal, completed - with the GPU's answer
// injected, so no device is needed. (The boolean rule alone is not what this checks.)
#include "nr_pe_d3d12split.hpp"

#include <cstdio>
#include <map>

namespace split = nr::pe::split;

namespace {
struct Fake {
    // Per queue: the value the GPU is known to have finished.
    static std::map<ID3D12CommandQueue*, uint64_t>& done() { static std::map<ID3D12CommandQueue*, uint64_t> d; return d; }
};
ID3D12CommandQueue* const kQ = reinterpret_cast<ID3D12CommandQueue*>(uintptr_t(0x1000));
ID3D12CommandList* const kL = reinterpret_cast<ID3D12CommandList*>(uintptr_t(0x2000));
int bad = 0;

void expect(const char* what, bool got, bool want) {
    if (got != want) { std::printf("FAIL %s: got %d want %d\n", what, got ? 1 : 0, want ? 1 : 0); ++bad; }
    else std::printf("ok   %s\n", what);
}
}  // namespace

int main() {
    // Receipts hold their fence now, so completed() asks the query with the value only (queue null).
    split::set_fence_query_for_test([](ID3D12CommandQueue*, uint64_t v) { return Fake::done()[kQ] >= v; });

    // 1. A live recording that never ran is not finished.
    const uint64_t t1 = split::book(kL);
    expect("booked, never executed", split::completed(t1), false);

    // 2. Executed, the GPU has not reached it: still not finished; sealed or not.
    split::note_execution(kQ, 1, &kL);
    expect("executed, fence pending (live)", split::completed(t1), false);
    split::seal(t1);
    expect("executed, fence pending (sealed)", split::completed(t1), false);

    // 3. The fence passes: finished.
    Fake::done()[kQ] = 1;
    expect("executed, fence completed", split::completed(t1), true);

    // 4. A new generation after a successful seal; the old ticket keeps its own answer.
    const uint64_t t2 = split::book(kL);
    expect("new generation is a different ticket", t2 != t1 && t2 != 0, true);
    expect("new generation, never executed", split::completed(t2), false);
    split::note_execution(kQ, 1, &kL);
    expect("new generation, fence pending", split::completed(t2), false);
    Fake::done()[kQ] = 2;
    split::seal(t2);
    expect("new generation, fence completed + sealed", split::completed(t2), true);

    // 5. Executed twice: the later execution must finish before the recording counts as done.
    const uint64_t t3 = split::book(kL);
    split::note_execution(kQ, 1, &kL);   // value 3
    split::note_execution(kQ, 1, &kL);   // value 4
    split::seal(t3);
    Fake::done()[kQ] = 3;
    expect("two executions, first only", split::completed(t3), false);
    Fake::done()[kQ] = 4;
    expect("two executions, both done", split::completed(t3), true);

    // 6. Discarded after execution: the submitted work still holds its resources.
    const uint64_t t4 = split::book(kL);
    split::note_execution(kQ, 1, &kL);   // value 5
    split::invalidate(t4);
    Fake::done()[kQ] = 4;
    expect("discarded, execution pending", split::completed(t4), false);
    Fake::done()[kQ] = 5;
    expect("discarded, execution done", split::completed(t4), true);

    // 7. A submission carrying a list with no ticket touches nothing: no new generation, no receipt.
    ID3D12CommandList* const other = reinterpret_cast<ID3D12CommandList*>(uintptr_t(0x3000));
    split::note_execution(kQ, 1, &other);

    std::printf("%d failure(s)\n", bad);
    return bad ? 1 : 0;
}
