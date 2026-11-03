// Parameterized wave32 carrier for one ACO compute body (M2).
// TAG makes each record's DXIL distinct so every record gets its own PSDB key;
// NTX/NTY/NTZ match the record shell's LocalSize so the hardware TID packing the
// ACO body expects (including 2D 8x8) is what the carrier asks for.
// The root signature is one constant plus one UAV; the UAV is the descriptor
// table. Reading the constant and addressing the UAV keep both in the driver's
// user-data map; the ACO prefix replaces this code entirely.
#ifndef TAG
#define TAG 1
#endif
#ifndef NTX
#define NTX 32
#endif
#ifndef NTY
#define NTY 1
#endif
#ifndef NTZ
#define NTZ 1
#endif

cbuffer Params : register(b0) {
    uint tag;
};

RWByteAddressBuffer g_out : register(u0);

[WaveSize(32)]
[numthreads(NTX, NTY, NTZ)]
void main(uint3 tid : SV_DispatchThreadID) {
    // The UAV is the 4096-byte descriptor table. A full-grid store walks off it
    // and hangs the device when the ACO body is not the thing that runs.
    if (tid.x < 16)
        g_out.Store(tid.x * 4, tag + TAG);
}
