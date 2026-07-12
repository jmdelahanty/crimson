#include <metal_stdlib>
using namespace metal;

struct RasterData {
    float4 position [[position]];
    float2 texcoord;
};

vertex RasterData fullscreenVertex(uint vertex_id [[vertex_id]]) {
    constexpr float2 positions[] = {
        float2(-1.0, -1.0), float2(3.0, -1.0), float2(-1.0, 3.0)
    };
    constexpr float2 texcoords[] = {
        float2(0.0, 1.0), float2(2.0, 1.0), float2(0.0, -1.0)
    };
    return {float4(positions[vertex_id], 0.0, 1.0), texcoords[vertex_id]};
}

// The representative recording is limited-range 8-bit 4:2:0. BT.709 is the
// probe default when the file does not declare a matrix, matching Crimson's
// existing diagnostic assumption. Color parity must be validated separately.
fragment float4 nv12Fragment(RasterData in [[stage_in]],
                             texture2d<float> y_texture [[texture(0)]],
                             texture2d<float> uv_texture [[texture(1)]]) {
    constexpr sampler s(address::clamp_to_edge, filter::linear);
    float y = (y_texture.sample(s, in.texcoord).r - 16.0 / 255.0) *
              (255.0 / 219.0);
    float2 uv = uv_texture.sample(s, in.texcoord).rg - float2(0.5);
    float3 rgb;
    rgb.r = y + 1.5748 * uv.y;
    rgb.g = y - 0.1873 * uv.x - 0.4681 * uv.y;
    rgb.b = y + 1.8556 * uv.x;
    return float4(saturate(rgb), 1.0);
}
