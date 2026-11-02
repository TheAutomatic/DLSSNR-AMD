#include <cstdarg>
#include <cstdio>
namespace nr { namespace pe { void log(const char* f, ...) { va_list a; va_start(a, f); std::vfprintf(stderr, f, a); va_end(a); std::fputc('\n', stderr); } } }
namespace nr { namespace pe { int trace_execute() { return 0; } } }
