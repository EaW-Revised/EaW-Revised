// Original synthetic fixture for legacy flat vector-array initialization.
const float2 Offsets[4] =
{
    -1, 0,
     0, 1,
     1, 0,
     0, -1,
};

float4 aggregate_vs(float4 position : POSITION) : POSITION
{
    return position + float4(Offsets[0], 0, 0);
}

float4 aggregate_ps() : COLOR0
{
    return float4(1, 1, 1, 1);
}

technique max_viewport
{
    pass preview
    {
        VertexShader = compile vs_1_1 aggregate_vs();
        PixelShader = compile ps_1_1 aggregate_ps();
    }
}

technique runtime
{
    pass draw
    {
        VertexShader = compile vs_1_1 aggregate_vs();
        PixelShader = compile ps_1_1 aggregate_ps();
    }
}
