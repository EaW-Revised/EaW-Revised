// #include "not-active.fxh"
#include "common.FXH"

string _ALAMO_RENDER_PHASE = "Opaque";

/* technique CommentedOut {
    pass never_live { VertexShader = compile vs_1_1 synthetic_vs(); }
} */

technique Preferred < string LOD = "DX9"; > {
    pass p0 {
        ZWriteEnable = TRUE;
        AlphaBlendEnable = (Opacity < 1.0f);
        SrcBlend = SRCALPHA;
        DestBlend = INVSRCALPHA;
        VertexShader = compile vs_1_1 synthetic_vs();
        PixelShader = compile ps_2_0 synthetic_ps();
    }
}

technique Fixed < string LOD = "FIXEDFUNCTION"; > {
    pass draw {
        VertexShader = NULL;
        PixelShader = NULL;
        Texture[0] = <BaseSampler>;
        ColorOp[0] = MODULATE2X;
        ColorArg1[0] = DIFFUSE;
        ColorArg2[0] = TEXTURE;
        AlphaOp[0] = MODULATE;
        AlphaArg1[0] = DIFFUSE;
        AlphaArg2[0] = TEXTURE;
        ColorOp[1] = DISABLE;
    }
    pass cleanup {
        TextureTransformFlags[0] = DISABLE;
    }
}
