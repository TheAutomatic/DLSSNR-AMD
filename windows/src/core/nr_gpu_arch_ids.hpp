#pragma once
// The GPU architectures NR tells apart and the tables that tell them (no Vulkan, no Windows: the D3D12 side and the
// planner include this alone; nr_gpu_arch.hpp asks the GPU). See nr_gpu_arch.hpp for how a GPU is identified.
#include <cstdint>
#include <cstdio>
#include <string>

namespace nr::arch {

enum class GpuArch { unknown, rdna1, rdna2, rdna3, rdna4 };

inline const char* name(GpuArch a) {
    switch (a) {
    case GpuArch::rdna1: return "RDNA1";
    case GpuArch::rdna2: return "RDNA2";
    case GpuArch::rdna3: return "RDNA3";
    case GpuArch::rdna4: return "RDNA4";
    case GpuArch::unknown: break;
    }
    return "unknown";
}
// NR has a network for it (and its data in the package).
inline bool has_network(GpuArch a) {
    switch (a) {
    case GpuArch::rdna3: case GpuArch::rdna4: return true;
    case GpuArch::unknown: case GpuArch::rdna1: case GpuArch::rdna2: break;
    }
    return false;
}
// The folder of an architecture's ACO network inside dlssnr-amd\aco (RDNA4's is the bundle itself); null: none.
inline const char* aco_folder(GpuArch a) {
    switch (a) {
    case GpuArch::rdna4: return "";
    case GpuArch::rdna3: return "rdna3";
    case GpuArch::unknown: case GpuArch::rdna1: case GpuArch::rdna2: break;
    }
    return nullptr;
}

// EF_AMDGPU_MACH values (LLVM's BinaryFormat/ELF.h) and their architectures. The generic targets are named too: a
// driver that compiles for one of them still says which family it is.
struct Machine { uint8_t mach; const char* gfx; GpuArch arch; };
inline constexpr Machine kMachines[] = {
    {0x33, "gfx1010", GpuArch::rdna1}, {0x34, "gfx1011", GpuArch::rdna1}, {0x35, "gfx1012", GpuArch::rdna1},
    {0x42, "gfx1013", GpuArch::rdna1}, {0x52, "gfx10-1-generic", GpuArch::rdna1},
    {0x36, "gfx1030", GpuArch::rdna2}, {0x37, "gfx1031", GpuArch::rdna2}, {0x38, "gfx1032", GpuArch::rdna2},
    {0x39, "gfx1033", GpuArch::rdna2}, {0x3e, "gfx1034", GpuArch::rdna2}, {0x3d, "gfx1035", GpuArch::rdna2},
    {0x45, "gfx1036", GpuArch::rdna2}, {0x53, "gfx10-3-generic", GpuArch::rdna2},
    {0x41, "gfx1100", GpuArch::rdna3}, {0x46, "gfx1101", GpuArch::rdna3}, {0x47, "gfx1102", GpuArch::rdna3},
    {0x44, "gfx1103", GpuArch::rdna3}, {0x43, "gfx1150", GpuArch::rdna3}, {0x4a, "gfx1151", GpuArch::rdna3},
    {0x55, "gfx1152", GpuArch::rdna3}, {0x58, "gfx1153", GpuArch::rdna3}, {0x54, "gfx11-generic", GpuArch::rdna3},
    {0x48, "gfx1200", GpuArch::rdna4}, {0x4e, "gfx1201", GpuArch::rdna4}, {0x59, "gfx12-generic", GpuArch::rdna4},
};
inline const Machine* machine(uint32_t mach) {
    for (const Machine& m : kMachines)
        if (m.mach == (mach & 0xffu)) return &m;
    return nullptr;
}
inline GpuArch from_machine(uint32_t mach) {
    const Machine* m = machine(mach);
    return m ? m->arch : GpuArch::unknown;
}

// Exact PCI device IDs of AMD GPUs by architecture (pci.ids and libdrm's amdgpu.ids, 11-10). Graphics parts only: an
// Instinct accelerator or an unlisted ID is unknown.
struct Device { uint16_t id; GpuArch arch; };
inline constexpr Device kDevices[] = {
    // RDNA4: Navi 48 (RX 9070 / 9070 XT / 9070 GRE, Radeon AI PRO R9700), Navi 44 (RX 9060 XT)
    {0x7550, GpuArch::rdna4}, {0x7551, GpuArch::rdna4}, {0x7590, GpuArch::rdna4},
    // RDNA3: Navi 31 (RX 7900, PRO W7800 / W7900)
    {0x7448, GpuArch::rdna3}, {0x7449, GpuArch::rdna3}, {0x744a, GpuArch::rdna3}, {0x744b, GpuArch::rdna3},
    {0x744c, GpuArch::rdna3}, {0x745e, GpuArch::rdna3},
    // Navi 32 (RX 7700 XT / 7800 XT, PRO W7700, PRO V710)
    {0x7460, GpuArch::rdna3}, {0x7461, GpuArch::rdna3}, {0x7470, GpuArch::rdna3}, {0x747e, GpuArch::rdna3},
    // Navi 33 (RX 7600 family, 7600M XT, 7400 / 7300, 7500, PRO W7400 / W7500 / W7600)
    {0x73f0, GpuArch::rdna3}, {0x7480, GpuArch::rdna3}, {0x7481, GpuArch::rdna3}, {0x7483, GpuArch::rdna3},
    {0x7487, GpuArch::rdna3}, {0x7489, GpuArch::rdna3}, {0x748b, GpuArch::rdna3}, {0x7499, GpuArch::rdna3},
    {0x749f, GpuArch::rdna3},
    // RDNA3 / 3.5 integrated: Phoenix (780M / 760M / 740M), Hawk Point (780M / 740M / 840M), Strix Point (880M / 890M),
    // Strix Halo (8040S / 8050S / 8060S), Krackan Point (840M / 860M)
    {0x15bf, GpuArch::rdna3}, {0x15c8, GpuArch::rdna3}, {0x1900, GpuArch::rdna3}, {0x1901, GpuArch::rdna3},
    {0x1902, GpuArch::rdna3}, {0x150e, GpuArch::rdna3}, {0x1586, GpuArch::rdna3}, {0x1114, GpuArch::rdna3},
    // RDNA2: Navi 21 / 22 / 23 / 24 (RX 6000, PRO W6000, V620)
    {0x73a1, GpuArch::rdna2}, {0x73a2, GpuArch::rdna2}, {0x73a3, GpuArch::rdna2}, {0x73a5, GpuArch::rdna2},
    {0x73ab, GpuArch::rdna2}, {0x73ae, GpuArch::rdna2}, {0x73af, GpuArch::rdna2}, {0x73bf, GpuArch::rdna2},
    {0x73c3, GpuArch::rdna2}, {0x73ce, GpuArch::rdna2}, {0x73df, GpuArch::rdna2},
    {0x73e0, GpuArch::rdna2}, {0x73e1, GpuArch::rdna2}, {0x73e3, GpuArch::rdna2}, {0x73ef, GpuArch::rdna2},
    {0x73ff, GpuArch::rdna2},
    {0x7421, GpuArch::rdna2}, {0x7422, GpuArch::rdna2}, {0x7423, GpuArch::rdna2}, {0x7424, GpuArch::rdna2},
    {0x743f, GpuArch::rdna2},
    // RDNA2 integrated: Van Gogh (Steam Deck), Sephiroth (Steam Deck OLED), Rembrandt (680M / 660M), Raphael / Granite
    // Ridge (Radeon Graphics), Mendocino (610M)
    {0x163f, GpuArch::rdna2}, {0x1435, GpuArch::rdna2}, {0x1681, GpuArch::rdna2}, {0x164e, GpuArch::rdna2},
    {0x13c0, GpuArch::rdna2}, {0x1506, GpuArch::rdna2},
    // RDNA1: Navi 10 / 12 / 14 (RX 5000, PRO W5000)
    {0x7310, GpuArch::rdna1}, {0x7312, GpuArch::rdna1}, {0x7319, GpuArch::rdna1}, {0x731b, GpuArch::rdna1},
    {0x731f, GpuArch::rdna1}, {0x7340, GpuArch::rdna1}, {0x7341, GpuArch::rdna1}, {0x7347, GpuArch::rdna1},
    {0x734f, GpuArch::rdna1}, {0x7360, GpuArch::rdna1}, {0x7362, GpuArch::rdna1},
};
inline GpuArch from_device_id(uint32_t vendor, uint32_t device) {
    if (vendor != 0x1002) return GpuArch::unknown;
    for (const Device& d : kDevices)
        if (d.id == device) return d.arch;
    return GpuArch::unknown;
}

// What identify found: the architecture, the machine the driver's compiler named (0: it could not be asked), and how
// (for the log).
struct Identity {
    GpuArch arch = GpuArch::unknown;
    uint32_t mach = 0;
    std::string how;
};
// The line for a log or a refusal: "RDNA3 (gfx1103; the driver's compiler)" or "unknown (PCI 1002:abcd, not listed)".
inline std::string describe(const Identity& id) {
    return std::string(name(id.arch)) + " (" + id.how + ")";
}

// The decision from what could be found: the machine the driver's compiler named (0: not asked, `asked` says why)
// and the PCI IDs (`ids`: where they came from; null: none). The machine is the answer whenever there is one; the
// PCI table only without it. A PCI entry that disagrees with the machine is logged beside it, never used over it.
inline Identity decide(uint32_t mach, const std::string& asked, const char* ids, uint32_t vendor, uint32_t device) {
    Identity r;
    r.mach = mach & 0xffu;
    char text[160];
    const GpuArch listed = ids ? from_device_id(vendor, device) : GpuArch::unknown;
    if (r.mach) {
        r.arch = from_machine(r.mach);
        const Machine* m = machine(r.mach);
        if (m) std::snprintf(text, sizeof text, "%s, ", m->gfx);
        else std::snprintf(text, sizeof text, "machine 0x%02x, not in NR's table, ", unsigned(r.mach));
        r.how = text + std::string("from ") + asked;
        if (listed != GpuArch::unknown && listed != r.arch) {
            std::snprintf(text, sizeof text, "; PCI %04x:%04x is listed as %s", unsigned(vendor), unsigned(device),
                          name(listed));
            r.how += text;
        }
    } else if (ids) {
        r.arch = listed;
        std::snprintf(text, sizeof text, "PCI %04x:%04x as %s, %s", unsigned(vendor), unsigned(device), ids,
                      vendor != 0x1002 ? "not AMD's, or spoofed (OptiScaler's GPU spoofing reports 10de:2684)" : listed == GpuArch::unknown ? "not in NR's list" : "listed");
        r.how = text + std::string("; the driver's compiler could not be asked: ") + asked;
    } else {
        r.how = "no PCI IDs; the driver's compiler could not be asked: " + asked;
    }
    return r;
}

}  // namespace nr::arch
