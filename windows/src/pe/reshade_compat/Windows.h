// ReShade includes <Windows.h>; mingw on a case-sensitive filesystem has <windows.h>.
// include_next, not a plain include: on a case-insensitive filesystem (building on Windows)
// <windows.h> also resolves to this shim, and a plain include would either recurse into it
// or, with the pragma, skip the real header entirely - DWORD/BOOL & co. then undefined.
#pragma once
#include_next <windows.h>
