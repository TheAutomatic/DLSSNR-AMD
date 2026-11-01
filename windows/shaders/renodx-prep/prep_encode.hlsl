// RenoDX's v6 encode (v6_encode.cs_5_1.hlsl) for a frame Preprocess has already put through its own display curve
// ([Preprocess] Curve other than none): the same area footprint onto the network's extent and the same sRGB encode,
// without RenoDX's shadow toe and highlight shoulder, so the frame is not given two curves. Divisor 1.
// windows/shaders/renodx-prep/renodx_prep_encode.comp (ours) as the D3D12 HLSL the ASD route compiles at run time:
// Original at t0 (binding 0), the proxy at u0 (binding 4), the push block in b0.
Texture2D<float4> PrepOriginal : register(t0);
RWTexture2D<float4> PrepOutput : register(u0);

cbuffer PrepEncodeC : register(b0) {
    uint pe_size_x;
    uint pe_size_y;
    uint pe_source_x;
    uint pe_source_y;
};

float3 pe_srgb_encode(float3 c) {   // codec_math.hlsli SrgbEncode
    c = clamp(c, 0.0f, 1.0f);
    return lerp(1.055f * pow(c, 1.0f / 2.4f) - 0.055f, c * 12.92f, step(c, 0.0031308f));
}

[numthreads(16, 16, 1)]
void main(uint3 dispatch_id : SV_DispatchThreadID) {
    const uint2 pixel = dispatch_id.xy;
    if (pixel.x >= pe_size_x || pixel.y >= pe_size_y) return;
    const uint2 source_size = max(uint2(pe_source_x, pe_source_y), uint2(1u, 1u));
    const float2 scale = float2(source_size) / float2(pe_size_x, pe_size_y);
    const float2 center = (float2(pixel) + 0.5f) * scale;
    const float2 footprint_min = max(center - 0.5f * scale, 0.0f);
    const float2 footprint_max = min(center + 0.5f * scale, float2(source_size));
    const int2 first = int2(floor(footprint_min));
    const int2 last = int2(ceil(footprint_max));
    float3 sum = 0.0f;
    float weight = 0.0f;
    for (int y = first.y; y < last.y; ++y) {
        for (int x = first.x; x < last.x; ++x) {
            const float2 overlap = min(float2(x + 1, y + 1), footprint_max) - max(float2(x, y), footprint_min);
            const float w = max(overlap.x, 0.0f) * max(overlap.y, 0.0f);
            const float3 texel = PrepOriginal.Load(int3(x, y, 0)).rgb;
            const bool3 inf = (asuint(texel) & 0x7f800000u) == 0x7f800000u;
            const float3 finite = inf ? (texel > 0.0f ? 65504.0f : 0.0f) : texel;
            sum += finite * w;
            weight += w;
        }
    }
    const float3 proxy = weight > 0.0f ? max(sum / weight, 0.0f) : 0.0f;
    PrepOutput[pixel] = float4(pe_srgb_encode(proxy), 1.0f);
}
