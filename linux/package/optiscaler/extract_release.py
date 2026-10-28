#!/usr/bin/env python3
"""Extract an OptiScaler release archive into a game directory.

Not zipfile.extractall(). The archive was made on Windows and its entry names carry backslashes --
"OptiScaler\\libxess.dll", "Licenses\\XeSS_LICENSE.txt" -- which extractall() takes as part of the
file name rather than as a separator. It produces one flat file literally called
"OptiScaler\\libxess.dll", and then Wine, asked by OptiScaler for OptiScaler\\libxess.dll, translates
that to OptiScaler/libxess.dll and finds nothing there. The separator is normalised here, and entries
are checked for path traversal on the way through.
"""

import os
import sys
import zipfile


def main(src: str, dst: str) -> int:
    root = os.path.normpath(dst)
    count = 0
    with zipfile.ZipFile(src) as z:
        for info in z.infolist():
            name = info.filename.replace("\\", "/")
            target = os.path.normpath(os.path.join(root, name))
            if target != root and not target.startswith(root + os.sep):
                print(f"archive entry escapes the target directory: {info.filename}", file=sys.stderr)
                return 1
            if name.endswith("/") or info.is_dir():
                os.makedirs(target, exist_ok=True)
                continue
            os.makedirs(os.path.dirname(target), exist_ok=True)
            with z.open(info) as fsrc, open(target, "wb") as fdst:
                fdst.write(fsrc.read())
            count += 1
    print(f"   extracted {count} files from {src}")
    return 0


if __name__ == "__main__":
    if len(sys.argv) != 3:
        print(f"usage: {sys.argv[0]} <archive.zip> <game-dir>", file=sys.stderr)
        raise SystemExit(2)
    raise SystemExit(main(sys.argv[1], sys.argv[2]))
