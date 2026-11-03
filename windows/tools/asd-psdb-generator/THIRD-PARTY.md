# Third-party binaries

This package contains **no** third-party binaries. The generator needs three
external executables/libraries at build and run time; obtain them yourself and
keep them outside the delivered tree (or under `third_party/`, which is not part
of the source zip).

| component | version used for `artifacts/0082` | source | sha256 of the file used |
|---|---|---|---|
| `D3D12StateObjectCompiler.dll` | Agility SDK 1.619.5 | NuGet `Microsoft.Direct3D.D3D12` 1.619.5, `build/native/bin/x64/` — https://www.nuget.org/packages/Microsoft.Direct3D.D3D12/1.619.5 | `5ca0591e4a20b27d69734940fb61f61307a3cf5c4dbfdfca3d8545005a57a6a6` |
| `D3D12Core.dll` | Agility SDK 1.619.5 | same package, same directory | `eddf4cff4eda8162624b88694ad2adf4b09bc5aee6339191f39adf8ae48b41e7` |
| `amdxc_compiler.dll` | AMD ASD compiler plugin 32.0.32015.7029 | NuGet `amd.asdcompilerplugin` 32.0.32015.7029, `build/native/bin/x64/` — https://www.nuget.org/packages/amd.asdcompilerplugin/32.0.32015.7029 | `d2c3f76277bab61ffc26ad1583695c6ed1c6500d8b93b9ab2750bb0c347f5555` |
| `dxc.exe` | Windows SDK 10.0.26100.0 | `C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0\x64\dxc.exe` | `854d5f29dbbe25b584942855e65d673db3164f7f68e20fb784be9cd747d14814` |

The NuGet SHA-256 of the packages themselves (recorded by the lab):

- `microsoft.direct3d.d3d12.1.619.5.nupkg` — `0E9BCF32AAC9A79343EDE9B21E4864950EE54577E3D8E19BFCDF002BB4E9BFD6`
- `amd.asdcompilerplugin.32.0.32015.7029.nupkg` — `19C1A6E41DF2F340520169BC555172486D46A9C05F15F0C2A968F57E1CA0ACF3`

## Redistribution

No Microsoft or AMD DLL or SDK executable is included. Obtain each dependency from the source listed above and follow its own package license. DirectX header licensing alone does not establish redistribution rights for runtime or compiler binaries. This handoff makes no blanket redistribution claim.

The generated data files are included; player/runtime compatibility must be checked separately from the generator's build dependencies.
