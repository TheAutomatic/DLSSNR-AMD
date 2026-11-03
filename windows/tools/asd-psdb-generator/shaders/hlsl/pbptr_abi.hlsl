// ABI carrier for the gfx1201 g_pbptr load. The driver compiles this so the
// root signature has 11 constants and one UAV. The hook replaces the ELF body
// with the ACO kernel before the parser keeps it. This HLSL is not the result.
cbuffer Params : register(b0) {
    uint w0;
    uint w1;
    uint w2;
    uint w3;
    uint w4;
    uint w5;
    uint w6;
    uint w7;
    uint w8;
    uint w9;
    uint w10;
};

RWByteAddressBuffer g_out : register(u0);

[numthreads(64, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID) {
    uint acc = w0 + w1 + w2 + w3 + w4 + w5 + w6 + w7 + w8 + w9 + w10;
    g_out.Store(tid.x * 4, acc ^ tid.x);
}
