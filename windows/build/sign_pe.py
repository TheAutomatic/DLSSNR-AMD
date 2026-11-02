#!/usr/bin/env python3
"""Self-signed Authenticode signatures on a package's Windows binaries (NR_SIGN=1 in build_package.sh).

Some games load only DLLs that carry an Authenticode signature, and check that it is there, not who made it. Every PE
file in the folder (.dll .exe .asi .ocx and ReShade's .addon / .addon32 / .addon64) without an embedded signature is
signed with a self-signed code-signing certificate made for this run (RSA 3072, SHA-256; the key is thrown away after,
nothing is kept or committed). A binary that already carries a signature (AMD's, Intel's, ReShade's) is left as it is.
The certificate is not a trusted publisher and nothing asks the user to trust it: Windows shows the files as signed by
an unknown publisher.

After signing, each file must equal the original byte for byte apart from the PE checksum, the security directory
entry and the certificate appended at the end (after zero padding to 8 bytes), and osslsigncode must verify the
signature against the run's certificate; otherwise the package build stops.

  sign_pe.py <folder>      (needs openssl and osslsigncode)
"""
import os
import shutil
import struct
import subprocess
import sys
import tempfile

EXTENSIONS = ('.dll', '.exe', '.asi', '.ocx', '.addon', '.addon32', '.addon64')
SUBJECT = '/CN=DLSSNR-AMD (self-signed)'


def pe_fields(data):
    """(checksum offset, security directory offset) of a PE image, or None when it is not one."""
    if len(data) < 256 or data[:2] != b'MZ':
        return None
    pe = struct.unpack_from('<I', data, 0x3c)[0]
    if pe < 64 or pe + 184 > len(data) or data[pe:pe + 4] != b'PE\0\0':
        return None
    opt = pe + 24
    magic = struct.unpack_from('<H', data, opt)[0]
    if magic not in (0x10b, 0x20b):
        return None
    return opt + 64, opt + (144 if magic == 0x20b else 128)


def check_payload(before, after, fields):
    checksum, security = fields
    start, size = struct.unpack_from('<II', after, security)
    aligned = (len(before) + 7) & ~7
    if start != aligned or size < 8 or start + size != len(after):
        raise RuntimeError('the signature is not the appended certificate alone')
    if any(after[len(before):start]):
        raise RuntimeError('non-zero padding before the certificate')
    norm = bytearray(after[:len(before)])
    norm[checksum:checksum + 4] = before[checksum:checksum + 4]
    norm[security:security + 8] = before[security:security + 8]
    if bytes(norm) != before:
        raise RuntimeError('signing changed the program itself')


def main():
    if len(sys.argv) != 2 or not os.path.isdir(sys.argv[1]):
        sys.exit(__doc__)
    root = sys.argv[1]
    for tool in ('openssl', 'osslsigncode'):
        if not shutil.which(tool):
            sys.exit(f'sign_pe.py: {tool} not found')
    files = sorted(os.path.join(d, f) for d, _, fs in os.walk(root) for f in fs if f.lower().endswith(EXTENSIONS))
    # scratch beside the folder (the build's own directory), not the system's temporary folder
    with tempfile.TemporaryDirectory(dir=os.path.dirname(os.path.abspath(root))) as tmp:
        key, cert = os.path.join(tmp, 'key.pem'), os.path.join(tmp, 'cert.pem')
        subprocess.run(['openssl', 'req', '-x509', '-newkey', 'rsa:3072', '-sha256', '-days', '1826', '-nodes',
                        '-subj', SUBJECT, '-addext', 'keyUsage=critical,digitalSignature',
                        '-addext', 'extendedKeyUsage=codeSigning', '-keyout', key, '-out', cert],
                       check=True, capture_output=True)
        signed = kept = 0
        for path in files:
            data = open(path, 'rb').read()
            fields = pe_fields(data)
            if not fields:
                continue
            if struct.unpack_from('<Q', data, fields[1])[0]:   # a signature of its own: left as it is
                kept += 1
                print(f'kept (signed already): {os.path.relpath(path, root)}')
                continue
            out = os.path.join(tmp, 'signed')
            if os.path.exists(out):
                os.remove(out)   # (osslsigncode does not write over a file)
            r = subprocess.run(['osslsigncode', 'sign', '-certs', cert, '-key', key, '-h', 'sha256', '-n', 'DLSSNR-AMD',
                                '-in', path, '-out', out], capture_output=True, text=True)
            if r.returncode != 0:
                raise RuntimeError(f'{path}: osslsigncode sign failed:\n{r.stdout}{r.stderr}')
            after = open(out, 'rb').read()
            check_payload(data, after, fields)
            v = subprocess.run(['osslsigncode', 'verify', '-CAfile', cert, '-in', out], capture_output=True, text=True)
            if v.returncode != 0:
                raise RuntimeError(f'{path}: the signature does not verify:\n{v.stdout}{v.stderr}')
            with open(path, 'wb') as f:   # in place (the file keeps its mode)
                f.write(after)
            signed += 1
            print(f'signed: {os.path.relpath(path, root)}')
        print(f'sign_pe.py: {signed} signed, {kept} kept with their own signature')


if __name__ == '__main__':
    main()
