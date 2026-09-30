// Legacy Direct3D 9 synthetic draw oracle for the TerrainWater t1 ps_1_1 semantics.
//
// Original clean-room harness: the pixel programs below are hand-encoded ps.1.1 token
// streams built from the public d3d9types.h constants, not assembled from any
// Petroglyph file.  Input is the whitespace-separated case file produced by cases.py;
// output is JSON with the adapter identity and one RGBA8 readback per case.
//
// Build (Windows only, MSVC):
//   cl /nologo /O2 /EHsc /W4 d3d9_oracle.cpp d3d9.lib user32.lib version.lib
// Run:
//   d3d9_oracle.exe <cases.txt> <readback.json>

#define _CRT_SECURE_NO_WARNINGS
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d9.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

DWORD reg(D3DSHADER_PARAM_REGISTER_TYPE type, DWORD number) {
    const DWORD t = static_cast<DWORD>(type);
    return 0x80000000u | (number & D3DSP_REGNUM_MASK) |
           ((t << D3DSP_REGTYPE_SHIFT) & D3DSP_REGTYPE_MASK) |
           ((t << D3DSP_REGTYPE_SHIFT2) & D3DSP_REGTYPE_MASK2);
}
DWORD dst(D3DSHADER_PARAM_REGISTER_TYPE type, DWORD number, DWORD mask = D3DSP_WRITEMASK_ALL) {
    return reg(type, number) | mask;
}
DWORD src(D3DSHADER_PARAM_REGISTER_TYPE type, DWORD number) {
    return reg(type, number) | D3DSP_NOSWIZZLE | D3DSPSM_NONE;
}

// tex t0; texbem t1, t0; tex t2; mad r0, v0, t1, v1; mul r0.rgb, r0, t2
std::vector<DWORD> t1_program() {
    return {
        D3DPS_VERSION(1, 1),
        D3DSIO_TEX, dst(D3DSPR_TEXTURE, 0),
        D3DSIO_TEXBEM, dst(D3DSPR_TEXTURE, 1), src(D3DSPR_TEXTURE, 0),
        D3DSIO_TEX, dst(D3DSPR_TEXTURE, 2),
        D3DSIO_MAD, dst(D3DSPR_TEMP, 0), src(D3DSPR_INPUT, 0), src(D3DSPR_TEXTURE, 1), src(D3DSPR_INPUT, 1),
        D3DSIO_MUL, dst(D3DSPR_TEMP, 0, D3DSP_WRITEMASK_0 | D3DSP_WRITEMASK_1 | D3DSP_WRITEMASK_2),
            src(D3DSPR_TEMP, 0), src(D3DSPR_TEXTURE, 2),
        D3DSIO_END,
    };
}

// tex t1; mov r0, t1  -- probe of stage-1 sampler state only.
std::vector<DWORD> stage1_probe_program() {
    return {
        D3DPS_VERSION(1, 1),
        D3DSIO_TEX, dst(D3DSPR_TEXTURE, 1),
        D3DSIO_MOV, dst(D3DSPR_TEMP, 0), src(D3DSPR_TEXTURE, 1),
        D3DSIO_END,
    };
}

struct Vertex {
    float x, y, z, rhw;
    DWORD diffuse, specular;
    float u0, v0, u1, v1, u2, v2;
};
const DWORD kFvf = D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_SPECULAR | D3DFVF_TEX3;

struct Texture { int format, width, height; std::vector<int> texels; };
struct Stage { int address_u, address_v, min_filter, mag_filter; };
struct Draw { int kind, zfunc, zwrite, blend, cleanup_after; std::vector<Vertex> vertices; };
struct Case {
    char name[128];
    int width, height;
    int clear[4];
    float clear_z;
    Texture textures[3];
    Stage stages[3];
    float bumpenv[4];
    std::vector<Draw> draws;
};

[[noreturn]] void fail(const char* what, HRESULT hr = S_OK) {
    std::fprintf(stderr, "d3d9_oracle: %s (hr=0x%08lx)\n", what, static_cast<unsigned long>(hr));
    std::exit(2);
}

void check(HRESULT hr, const char* what) {
    if (FAILED(hr)) fail(what, hr);
}

int read_int(FILE* f) {
    int v;
    if (std::fscanf(f, "%d", &v) != 1) fail("malformed case file (int)");
    return v;
}
float read_float(FILE* f) {
    float v;
    if (std::fscanf(f, "%f", &v) != 1) fail("malformed case file (float)");
    return v;
}
DWORD read_colour(FILE* f) {
    int r = read_int(f), g = read_int(f), b = read_int(f), a = read_int(f);
    return D3DCOLOR_ARGB(a, r, g, b);
}

std::vector<Case> read_cases(const char* path) {
    FILE* f = std::fopen(path, "r");
    if (!f) fail("cannot open case file");
    char magic[64];
    int version = 0;
    if (std::fscanf(f, "%63s %d", magic, &version) != 2 || std::strcmp(magic, "EAWR_T1_CASES") || version != 1)
        fail("unexpected case file header");
    const int count = read_int(f);
    std::vector<Case> cases(static_cast<size_t>(count));
    for (Case& c : cases) {
        if (std::fscanf(f, "%127s", c.name) != 1) fail("missing case name");
        c.width = read_int(f);
        c.height = read_int(f);
        for (int& v : c.clear) v = read_int(f);
        c.clear_z = read_float(f);
        for (Texture& t : c.textures) {
            t.format = read_int(f);
            t.width = read_int(f);
            t.height = read_int(f);
            const int channels = t.format == 0 ? 2 : 4;
            t.texels.resize(static_cast<size_t>(t.width * t.height * channels));
            for (int& v : t.texels) v = read_int(f);
        }
        for (Stage& s : c.stages) {
            s.address_u = read_int(f);
            s.address_v = read_int(f);
            s.min_filter = read_int(f);
            s.mag_filter = read_int(f);
        }
        for (float& m : c.bumpenv) m = read_float(f);
        const int draws = read_int(f);
        c.draws.resize(static_cast<size_t>(draws));
        for (Draw& d : c.draws) {
            d.kind = read_int(f);
            d.zfunc = read_int(f);
            d.zwrite = read_int(f);
            d.blend = read_int(f);
            d.cleanup_after = read_int(f);
            d.vertices.resize(static_cast<size_t>(read_int(f)));
            for (Vertex& v : d.vertices) {
                // Modern pixel-centre convention in the file; D3D9 centres are on integers.
                v.x = read_float(f) - 0.5f;
                v.y = read_float(f) - 0.5f;
                v.z = read_float(f);
                v.rhw = read_float(f);
                v.diffuse = read_colour(f);
                v.specular = read_colour(f);
                v.u0 = read_float(f); v.v0 = read_float(f);
                v.u1 = read_float(f); v.v1 = read_float(f);
                v.u2 = read_float(f); v.v2 = read_float(f);
            }
        }
    }
    std::fclose(f);
    return cases;
}

IDirect3DTexture9* make_texture(IDirect3DDevice9* device, const Texture& t) {
    IDirect3DTexture9* texture = nullptr;
    const D3DFORMAT format = t.format == 0 ? D3DFMT_V8U8 : D3DFMT_A8R8G8B8;
    check(device->CreateTexture(t.width, t.height, 1, 0, format, D3DPOOL_MANAGED, &texture, nullptr),
          "CreateTexture");
    D3DLOCKED_RECT locked;
    check(texture->LockRect(0, &locked, nullptr, 0), "LockRect texture");
    for (int y = 0; y < t.height; ++y) {
        unsigned char* row = static_cast<unsigned char*>(locked.pBits) + y * locked.Pitch;
        for (int x = 0; x < t.width; ++x) {
            const size_t i = static_cast<size_t>(y * t.width + x);
            if (t.format == 0) {
                // D3DFMT_V8U8: low byte U (du), high byte V (dv), two's complement.
                row[x * 2 + 0] = static_cast<unsigned char>(static_cast<signed char>(t.texels[i * 2 + 0]));
                row[x * 2 + 1] = static_cast<unsigned char>(static_cast<signed char>(t.texels[i * 2 + 1]));
            } else {
                // D3DFMT_A8R8G8B8 in memory: B, G, R, A.
                row[x * 4 + 0] = static_cast<unsigned char>(t.texels[i * 4 + 2]);
                row[x * 4 + 1] = static_cast<unsigned char>(t.texels[i * 4 + 1]);
                row[x * 4 + 2] = static_cast<unsigned char>(t.texels[i * 4 + 0]);
                row[x * 4 + 3] = static_cast<unsigned char>(t.texels[i * 4 + 3]);
            }
        }
    }
    texture->UnlockRect(0);
    return texture;
}

DWORD float_bits(float value) {
    DWORD bits;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

std::string file_version(const wchar_t* module) {
    DWORD handle = 0;
    const DWORD size = GetFileVersionInfoSizeW(module, &handle);
    if (!size) return "unknown";
    std::vector<unsigned char> data(size);
    if (!GetFileVersionInfoW(module, 0, size, data.data())) return "unknown";
    VS_FIXEDFILEINFO* info = nullptr;
    UINT length = 0;
    if (!VerQueryValueW(data.data(), L"\\", reinterpret_cast<void**>(&info), &length) || !info) return "unknown";
    char text[64];
    std::snprintf(text, sizeof(text), "%u.%u.%u.%u", HIWORD(info->dwFileVersionMS), LOWORD(info->dwFileVersionMS),
                  HIWORD(info->dwFileVersionLS), LOWORD(info->dwFileVersionLS));
    return text;
}

std::string json_escape(const char* text) {
    std::string out;
    for (const char* p = text; *p; ++p) {
        if (*p == '"' || *p == '\\') out += '\\';
        if (static_cast<unsigned char>(*p) >= 0x20) out += *p;
    }
    return out;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 3) {
        std::fprintf(stderr, "usage: d3d9_oracle <cases.txt> <readback.json>\n");
        return 2;
    }
    const std::vector<Case> cases = read_cases(argv[1]);

    WNDCLASSW wc = {};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"EawrT1Oracle";
    RegisterClassW(&wc);
    HWND hwnd = CreateWindowW(wc.lpszClassName, L"eawr-t1-oracle", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64,
                              nullptr, nullptr, wc.hInstance, nullptr);
    if (!hwnd) fail("CreateWindow");

    IDirect3D9* d3d = Direct3DCreate9(D3D_SDK_VERSION);
    if (!d3d) fail("Direct3DCreate9");
    D3DADAPTER_IDENTIFIER9 identifier = {};
    check(d3d->GetAdapterIdentifier(D3DADAPTER_DEFAULT, 0, &identifier), "GetAdapterIdentifier");
    D3DDISPLAYMODE mode = {};
    check(d3d->GetAdapterDisplayMode(D3DADAPTER_DEFAULT, &mode), "GetAdapterDisplayMode");
    const HRESULT v8u8 = d3d->CheckDeviceFormat(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, mode.Format, 0,
                                                D3DRTYPE_TEXTURE, D3DFMT_V8U8);
    if (FAILED(v8u8)) fail("D3DFMT_V8U8 textures unsupported", v8u8);

    D3DPRESENT_PARAMETERS pp = {};
    pp.Windowed = TRUE;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.BackBufferFormat = D3DFMT_UNKNOWN;
    pp.BackBufferWidth = 64;
    pp.BackBufferHeight = 64;
    pp.hDeviceWindow = hwnd;
    IDirect3DDevice9* device = nullptr;
    check(d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd,
                            D3DCREATE_HARDWARE_VERTEXPROCESSING | D3DCREATE_FPU_PRESERVE, &pp, &device),
          "CreateDevice");
    D3DCAPS9 caps = {};
    check(device->GetDeviceCaps(&caps), "GetDeviceCaps");
    if (caps.PixelShaderVersion < D3DPS_VERSION(1, 1)) fail("ps_1_1 unsupported");

    IDirect3DPixelShader9* t1_shader = nullptr;
    IDirect3DPixelShader9* probe_shader = nullptr;
    const std::vector<DWORD> t1_tokens = t1_program();
    const std::vector<DWORD> probe_tokens = stage1_probe_program();
    check(device->CreatePixelShader(t1_tokens.data(), &t1_shader), "CreatePixelShader t1");
    check(device->CreatePixelShader(probe_tokens.data(), &probe_shader), "CreatePixelShader probe");

    FILE* out = std::fopen(argv[2], "w");
    if (!out) fail("cannot open output");
    std::fprintf(out, "{\n \"schema\": \"eawr-t1-d3d9-readback/1\",\n");
    std::fprintf(out, " \"adapter\": {\"description\": \"%s\", \"driver\": \"%s\", \"driver_version\": \"%u.%u.%u.%u\", "
                      "\"vendor_id\": %lu, \"device_id\": %lu},\n",
                 json_escape(identifier.Description).c_str(), json_escape(identifier.Driver).c_str(),
                 HIWORD(identifier.DriverVersion.HighPart), LOWORD(identifier.DriverVersion.HighPart),
                 HIWORD(identifier.DriverVersion.LowPart), LOWORD(identifier.DriverVersion.LowPart),
                 static_cast<unsigned long>(identifier.VendorId), static_cast<unsigned long>(identifier.DeviceId));
    std::fprintf(out, " \"runtime\": {\"d3d9_dll\": \"%s\", \"device_type\": \"HAL\"},\n",
                 file_version(L"d3d9.dll").c_str());
    std::fprintf(out, " \"caps\": {\"pixel_shader_version\": \"%lu.%lu\", \"pixel_shader_1x_max_value\": %g},\n",
                 static_cast<unsigned long>(D3DSHADER_VERSION_MAJOR(caps.PixelShaderVersion)),
                 static_cast<unsigned long>(D3DSHADER_VERSION_MINOR(caps.PixelShaderVersion)),
                 caps.PixelShader1xMaxValue);
    std::fprintf(out, " \"cases\": [\n");

    for (size_t ci = 0; ci < cases.size(); ++ci) {
        const Case& c = cases[ci];
        IDirect3DSurface9* rt = nullptr;
        IDirect3DSurface9* depth = nullptr;
        IDirect3DSurface9* readback = nullptr;
        check(device->CreateRenderTarget(c.width, c.height, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &rt,
                                         nullptr), "CreateRenderTarget");
        check(device->CreateDepthStencilSurface(c.width, c.height, D3DFMT_D24S8, D3DMULTISAMPLE_NONE, 0, FALSE,
                                                &depth, nullptr), "CreateDepthStencilSurface");
        check(device->CreateOffscreenPlainSurface(c.width, c.height, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &readback,
                                                  nullptr), "CreateOffscreenPlainSurface");
        check(device->SetRenderTarget(0, rt), "SetRenderTarget");
        check(device->SetDepthStencilSurface(depth), "SetDepthStencilSurface");
        D3DVIEWPORT9 viewport = {0, 0, static_cast<DWORD>(c.width), static_cast<DWORD>(c.height), 0.0f, 1.0f};
        check(device->SetViewport(&viewport), "SetViewport");

        IDirect3DTexture9* textures[3] = {};
        for (int s = 0; s < 3; ++s) {
            textures[s] = make_texture(device, c.textures[s]);
            device->SetTexture(s, textures[s]);
            device->SetSamplerState(s, D3DSAMP_ADDRESSU, c.stages[s].address_u);
            device->SetSamplerState(s, D3DSAMP_ADDRESSV, c.stages[s].address_v);
            device->SetSamplerState(s, D3DSAMP_MINFILTER, c.stages[s].min_filter);
            device->SetSamplerState(s, D3DSAMP_MAGFILTER, c.stages[s].mag_filter);
            device->SetSamplerState(s, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
            device->SetSamplerState(s, D3DSAMP_SRGBTEXTURE, FALSE);
            device->SetTextureStageState(s, D3DTSS_TEXCOORDINDEX, s);
            device->SetTextureStageState(s, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
        }
        device->SetTextureStageState(1, D3DTSS_BUMPENVMAT00, float_bits(c.bumpenv[0]));
        device->SetTextureStageState(1, D3DTSS_BUMPENVMAT01, float_bits(c.bumpenv[1]));
        device->SetTextureStageState(1, D3DTSS_BUMPENVMAT10, float_bits(c.bumpenv[2]));
        device->SetTextureStageState(1, D3DTSS_BUMPENVMAT11, float_bits(c.bumpenv[3]));

        device->SetRenderState(D3DRS_ZENABLE, D3DZB_TRUE);
        device->SetRenderState(D3DRS_CULLMODE, D3DCULL_CCW);
        device->SetRenderState(D3DRS_FOGENABLE, FALSE);
        device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
        device->SetRenderState(D3DRS_LIGHTING, FALSE);
        device->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, FALSE);
        device->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
        device->SetRenderState(D3DRS_DITHERENABLE, FALSE);
        device->SetRenderState(D3DRS_SHADEMODE, D3DSHADE_GOURAUD);
        device->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD);
        device->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
        device->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
        device->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
        device->SetFVF(kFvf);
        check(device->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER,
                            D3DCOLOR_ARGB(c.clear[3], c.clear[0], c.clear[1], c.clear[2]), c.clear_z, 0), "Clear");
        check(device->BeginScene(), "BeginScene");
        for (const Draw& d : c.draws) {
            device->SetPixelShader(d.kind == 0 ? t1_shader : probe_shader);
            device->SetRenderState(D3DRS_ZFUNC, d.zfunc);
            device->SetRenderState(D3DRS_ZWRITEENABLE, d.zwrite);
            device->SetRenderState(D3DRS_ALPHABLENDENABLE, d.blend);
            check(device->DrawPrimitiveUP(D3DPT_TRIANGLELIST, static_cast<UINT>(d.vertices.size() / 3),
                                          d.vertices.data(), sizeof(Vertex)), "DrawPrimitiveUP");
            if (d.cleanup_after) {
                // Effect cleanup pass: AddressU[1] = AddressV[1] = WRAP.
                device->SetSamplerState(1, D3DSAMP_ADDRESSU, D3DTADDRESS_WRAP);
                device->SetSamplerState(1, D3DSAMP_ADDRESSV, D3DTADDRESS_WRAP);
            }
        }
        check(device->EndScene(), "EndScene");
        check(device->GetRenderTargetData(rt, readback), "GetRenderTargetData");
        D3DLOCKED_RECT locked;
        check(readback->LockRect(&locked, nullptr, D3DLOCK_READONLY), "LockRect readback");
        std::fprintf(out, "  {\"name\": \"%s\", \"width\": %d, \"height\": %d, \"rgba\": [", c.name, c.width, c.height);
        for (int y = 0; y < c.height; ++y) {
            const unsigned char* row = static_cast<const unsigned char*>(locked.pBits) + y * locked.Pitch;
            for (int x = 0; x < c.width; ++x) {
                std::fprintf(out, "%s%d,%d,%d,%d", (x || y) ? "," : "", row[x * 4 + 2], row[x * 4 + 1],
                             row[x * 4 + 0], row[x * 4 + 3]);
            }
        }
        readback->UnlockRect();
        std::fprintf(out, "]}%s\n", ci + 1 < cases.size() ? "," : "");
        for (IDirect3DTexture9* t : textures) {
            if (t) t->Release();
        }
        for (int s = 0; s < 3; ++s) device->SetTexture(s, nullptr);
        readback->Release();
        depth->Release();
        rt->Release();
    }
    std::fprintf(out, " ]\n}\n");
    std::fclose(out);
    probe_shader->Release();
    t1_shader->Release();
    device->Release();
    d3d->Release();
    DestroyWindow(hwnd);
    return 0;
}
