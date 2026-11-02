// The ticket test links the real split, which expects the product log and the trace counter; the ticket test has no
// product log, so it provides both here. (The harness itself defines its own log.)
#include <cstdarg>
#include <cstdio>
namespace nr { namespace pe {
void log(const char* f, ...) { va_list a; va_start(a, f); std::vfprintf(stderr, f, a); va_end(a); std::fputc('\n', stderr); }
int trace_execute() { return 0; }
} }
