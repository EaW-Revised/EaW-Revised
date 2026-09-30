#define SYNTHETIC_ENABLED 1
float4x4 ViewProjection : VIEWPROJECTION;
float4 Tint < string UIName = "Tint"; > = { 1, 0.5, 0.25, 1 };
texture BaseTexture;
sampler BaseSampler = sampler_state {
    Texture = <BaseTexture>;
    AddressU = Wrap;
    AddressV = Clamp;
    MinFilter = Linear;
};

struct VertexInput {
    float4 position : POSITION;
    float4 color : COLOR0;
    float2 uv : TEXCOORD0;
};
struct VertexOutput {
    float4 position : POSITION;
    float4 color : COLOR0;
    float2 uv : TEXCOORD0;
};

VertexOutput synthetic_vs(VertexInput input) {
    VertexOutput output;
    output.position = mul(input.position, ViewProjection);
    output.color = input.color * Tint;
    output.uv = input.uv;
    return output;
}

float4 synthetic_ps(VertexOutput input) : COLOR0 {
    return tex2D(BaseSampler, input.uv) * input.color;
}

