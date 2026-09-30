// Original synthetic fixture for corpus-discovered FX grammar gaps.
#define ENABLE_PATH 0
#if ENABLE_PATH // a legal trailing directive comment
float4 unused_path(float4 value) { return value * 2; }
#endif

float4 corpus_vs(float4 position : POSITION) : POSITION
{
    return position;
}

vertexshader corpus_vs_bin = compile vs_1_1 corpus_vs();

float4 corpus_ps() : COLOR0
{
    return float4(1, 1, 1, 1);
}

pixelshader corpus_ps_bin = compile ps_1_1 corpus_ps();

stateblock CorpusStates = stateblock_state
{
    ZWriteEnable = true;
    CullMode = CCW;
}

technique runtime < string LOD = "DX8"; >
{
    pass p0
    {
        VertexShader = (corpus_vs_bin);
        PixelShader = (corpus_ps_bin);
    }
}
