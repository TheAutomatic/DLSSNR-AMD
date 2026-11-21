# windows/build/x86 — 32-bit (PE32 i386) build

Compiles `dlssnr_amd.addon32`, the same add-on as `dlssnr_amd.addon64` but for 32-bit games
(DX9 through DXVK, DX10/11, Vulkan and D3D12 through vkd3d-proton).
Same UI, same networks, same ACO machine code; the add-on itself is native 32-bit, not bridged to a 64-bit process.

| Game runtime | 32-bit add-on |
| --- | --- |
| Vulkan, DX9 through DXVK | Supported |
| Native D3D10/11 | Supported; copies through bridge textures to NR's Vulkan device |
| D3D12 through vkd3d-proton | Supported |
| Native D3D12 | Not supported; requires command-list splitting |

```
windows/build/x86/build32.sh /path/to/minhook out
```

Use MinGW 13 from the `dlssnr-amd-build` Docker image, which provides floating-point
`std::from_chars` and the required Windows declarations.

What only the 32-bit build needs:

- `tc/gcc32`, `tc/gxx32`: i686 wrappers (`-D_WIN32_WINNT=0x0A00`).
- `vulkan_lazy32.c`/`.s`: Vulkan entry points resolved lazily, one tail jump per entry point, so
  `vulkan-1.dll` is not in the import table (a 32-bit game supplies the loader).
- `nr_device_chain_sizes.hpp`: the sizes worked out for the 32-bit layouts.

32-bit games are also the reason for three changes in the tree itself: the D3D12 split stubs
(`nr_pe_d3d12split.cpp`), `bufferDeviceAddress` being probed instead of assumed and the address
space report naming `LargeAddressAware` (`nr_pe_session.cpp`).
