#include "eawr/assets/effect.hpp"

#include <bit>
#include <iostream>
#include <span>
#include <string_view>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3dcompiler.h>
#endif

// EFX-11: our own invented source only. The optional native compiler supplies
// independent layout evidence; it is never a production or installed-data path.
int run_effect_native_probe() {
#ifdef _WIN32
    const auto library = LoadLibraryExW(L"d3dcompiler_47.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!library) { std::cout << "Native effect probe unavailable: system compiler absent\n"; return 0; }
    const auto compile = std::bit_cast<decltype(&D3DCompile)>(GetProcAddress(library, "D3DCompile"));
    if (!compile) { FreeLibrary(library); std::cerr << "Native compiler export absent\n"; return 1; }
    constexpr std::string_view source = R"(
float4 OwnedTint : InventedUnknownSemantic = float4(1, 0.25, 0, 1);
float OwnedClock : TIME = 0;
texture OwnedTexture;
sampler2D OwnedSampler = sampler_state {
    Texture = <OwnedTexture>; MinFilter = LINEAR; MagFilter = LINEAR; MipFilter = NONE;
};
float4 owned_vertex(float4 position : POSITION) : POSITION {
    return position + OwnedTint * cos(OwnedClock);
}
float4 owned_pixel(float2 uv : TEXCOORD0) : COLOR {
    return tex2D(OwnedSampler, uv) * OwnedTint;
}
vertexshader OwnedProgram = compile vs_2_0 owned_vertex();
technique OwnedTechnique {
    pass OwnedDraw {
        VertexShader = <OwnedProgram>;
        PixelShader = compile ps_2_0 owned_pixel();
        AlphaRef = (OwnedClock * 2);
    }
    pass OwnedReset {
        VertexShader = NULL; PixelShader = NULL; AlphaRef = 0;
    }
}
)";
    ID3DBlob* code = nullptr; ID3DBlob* errors = nullptr;
    const auto status = compile(source.data(), source.size(), "invented-effect", nullptr, nullptr,
        nullptr, "fx_2_0", 0U, 0U, &code, &errors);
    if (errors) {
        if (FAILED(status)) std::cerr << std::string_view(static_cast<const char*>(errors->GetBufferPointer()), errors->GetBufferSize());
        errors->Release();
    }
    int failures = 0;
    if (FAILED(status) || !code) { std::cerr << "Native invented effect compilation failed\n"; failures = 1; }
    else {
        const auto bytes = std::span(static_cast<const std::byte*>(code->GetBufferPointer()), code->GetBufferSize());
        const auto parsed = eawr::assets::read_compiled_effect(bytes);
        if (!parsed) {
            std::cerr << "Native invented effect: " << parsed.error().code << ' ' << parsed.error().message
                      << " column=" << parsed.error().column.value_or(0U) << '\n'; failures = 1;
        } else {
            const auto& effect = parsed.value(); std::size_t preshaders{}, constants{}, nulls{};
            // The compiler may move uniform-only arithmetic into PRES. Check
            // its inputs separately from the stage CTAB instead of assuming
            // the source uniforms all remain stage constants.
            const auto has_constant = [&](const auto& table, const auto& types, const std::string_view name,
                                          const eawr::assets::EffectRegisterSet registers,
                                          const std::uint16_t kind, const std::uint16_t columns) {
                for (const auto& constant : table) {
                    if (constant.name != name || constant.register_set != registers || constant.register_count != 1U ||
                        constant.type >= types.size() || !constant.parameter || *constant.parameter >= effect.parameters.size()) continue;
                    const auto& type = types[constant.type];
                    const auto& parameter = effect.parameters[*constant.parameter];
                    const auto& parameter_type = effect.types[effect.values[parameter.value].type];
                    if (type.kind == kind && type.rows == 1U && type.columns == columns &&
                        parameter_type.name == name && static_cast<std::uint32_t>(parameter_type.kind) == kind) return true;
                }
                return false;
            };
            bool pixel_sampler{}, pixel_tint{}, preshader_clock{}, preshader_tint{}, expression_clock{};
            for (const auto& program : effect.programs) {
                preshaders += program.preshaders.size(); constants += program.constants.size();
                if (program.stage == eawr::assets::EffectProgramStage::pixel) {
                    pixel_sampler |= has_constant(program.constants, program.constant_types, "OwnedSampler",
                        eawr::assets::EffectRegisterSet::sampler, 12U, 1U);
                    pixel_tint |= has_constant(program.constants, program.constant_types, "OwnedTint",
                        eawr::assets::EffectRegisterSet::floating4, 3U, 4U);
                }
                for (const auto& preshader : program.preshaders) {
                    preshader_clock |= has_constant(preshader.inputs, preshader.constant_types, "OwnedClock",
                        eawr::assets::EffectRegisterSet::floating4, 3U, 1U);
                    preshader_tint |= has_constant(preshader.inputs, preshader.constant_types, "OwnedTint",
                        eawr::assets::EffectRegisterSet::floating4, 3U, 4U);
                }
            }
            for (const auto& expression : effect.expressions)
                expression_clock |= has_constant(expression.inputs, expression.constant_types, "OwnedClock",
                    eawr::assets::EffectRegisterSet::floating4, 3U, 1U);
            for (const auto& state : effect.states)
                if (state.shader && state.shader->kind == eawr::assets::EffectShaderAssignmentKind::null_shader) ++nulls;
            if (effect.programs.size() != 2U || effect.expressions.empty() || !preshaders || nulls != 2U ||
                !pixel_sampler || !pixel_tint || !preshader_clock || !preshader_tint || !expression_clock) {
                std::cerr << "Native invented effect lacks expected descriptor controls\n"; failures = 1;
            }
            std::cout << "Native invented effect bytes=" << bytes.size() << " programs=" << effect.programs.size()
                      << " expressions=" << effect.expressions.size() << " preshaders=" << preshaders
                      << " constants=" << constants << " nulls=" << nulls << " pixel_sampler=" << pixel_sampler
                      << " pixel_tint=" << pixel_tint << " preshader_clock=" << preshader_clock
                      << " preshader_tint=" << preshader_tint << " expression_clock=" << expression_clock << '\n';
        }
    }
    if (code) code->Release();
    FreeLibrary(library);
    return failures;
#else
    return 0;
#endif
}
