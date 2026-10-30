#pragma once

#include <string>

// Where a game died. A vectored handler (games replace the unhandled-exception
// filter with their own crash reporters, so that one is not enough) logs the
// first few fatal-looking exceptions with module + offset and an unwound stack,
// and writes one minidump beside the game's executable: dlssnr-amd-crash.dmp.
// First-chance: an exception the game later handles is logged too, which is
// why the count is small and the log says "exception", not "crash".
namespace nr::pe {
void install_crash_watch();
void remove_crash_watch();
// "module+0xoffset" of an address, or the bare address when it is in no module.
std::string where_address(const void* at);
}
