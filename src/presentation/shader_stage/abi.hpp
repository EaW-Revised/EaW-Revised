#pragma once
// Independently declared binary interfaces, reviewed against the pinned public
// C headers. No upstream implementation or private C++ ABI is used here.
#include <cstddef>
#include <cstdint>

namespace eawr::presentation::shader_runtime::abi {
using U = std::uint32_t;
using H = void *;
using B = unsigned char;
struct Code {
    const void *data{};
    std::size_t size{};
};
// Pinned public vkd3d_shader.h:1804 macro; :1877 preprocess (type=7,
// enum :96); :1937 HLSL (type=6, enum :91). Header identity is in the lock.
struct Macro { const char *name{}, *value{}; };
struct Preprocess {
    int type{7};
    const void *next{};
    const Macro *macros{};
    U macro_count{};
    int (*open)(const char *, bool, const char *, void *, Code *){};
    void (*close)(const Code *, void *){};
    void *context{};
};
struct Hlsl {
    int type{6};
    const void *next{};
    const char *entry{};
    Code secondary;
    const char *profile{};
};
struct Option {
    int name{};
    U value{};
};
struct Compile {
    int type{};
    const void *next{};
    Code source;
    int source_type{3}, target_type{1};
    const Option *options{};
    U option_count{};
    int log_level{1};
    const char *source_name{};
};
struct Descriptor {
    int type{};
    U space{}, index{};
    int resource{}, data{};
    U flags{}, count{};
};
struct Descriptors {
    int type{2};
    const void *next{};
    Descriptor *data{};
    U count{};
};
struct Pair {
    U image_space{}, image{}, sampler_space{}, sampler{};
};
struct Pairs {
    int type{11};
    const void *next{};
    Pair *data{};
    U count{};
};
struct SignatureElement {
    const char *semantic{};
    U semantic_index{}, stream{};
    int sysval{}, component{};
    U index{}, mask{}, used_mask{};
    int precision{};
};
struct Signature {
    SignatureElement *data{};
    U count{};
};
struct Signatures {
    int type{9};
    const void *next{};
    Signature input, output, patch;
};
struct Binding {
    U set{}, binding{}, count{1};
};
struct ResourceBinding {
    int type{};
    U space{}, index{};
    int visibility{};
    U flags{};
    Binding target;
};
struct Interface {
    int type{1};
    const void *next{};
    const ResourceBinding *bindings{};
    U count{};
    const void *push_constants{};
    U push_count{};
    const void *combined{};
    U combined_count{};
    const void *counters{};
    U counter_count{};
};
struct Target {
    int type{4};
    const void *next{};
    const char *entry_point{"main"};
    int environment{2};
    const int *extensions{};
    U extension_count{};
    const void *parameters{};
    U parameter_count{};
    bool dual_source{};
    const U *output_swizzles{};
    U output_swizzle_count{};
};
struct Resource {
    U id{}, base_type{}, type{};
    const char *name{};
};
struct BuiltinResource {
    int builtin{};
    U value_type{};
    Resource resource;
};
struct Combined {
    U combined{}, image{}, sampler{};
};

#define EAWR_STAGE_VKD3D_API(X)                                                \
    X(int, vkd3d_shader_scan, (const Compile *, char **))                      \
    X(void, vkd3d_shader_free_scan_descriptor_info, (Descriptors *))           \
    X(void, vkd3d_shader_free_scan_combined_resource_sampler_info, (Pairs *))  \
    X(void, vkd3d_shader_free_scan_signature_info, (Signatures *))             \
    X(int, vkd3d_shader_compile, (const Compile *, Code *, char **))           \
    X(void, vkd3d_shader_free_shader_code, (Code *))                           \
    X(void, vkd3d_shader_free_messages, (char *))
#define EAWR_STAGE_CROSS_API(X)                                                \
    X(int, spvc_context_create, (H *))                                         \
    X(void, spvc_context_destroy, (H))                                         \
    X(int, spvc_context_parse_spirv, (H, const U *, std::size_t, H *))         \
    X(int, spvc_context_create_compiler, (H, int, H, int, H *))                \
    X(int, spvc_compiler_create_compiler_options, (H, H *))                    \
    X(int, spvc_compiler_options_set_uint, (H, int, U))                        \
    X(int, spvc_compiler_options_set_bool, (H, int, B))                        \
    X(int, spvc_compiler_install_compiler_options, (H, H))                     \
    X(int, spvc_compiler_compile, (H, const char **))                          \
    X(int, spvc_compiler_create_shader_resources, (H, H *))                    \
    X(int, spvc_resources_get_resource_list_for_type,                          \
      (H, int, const Resource **, std::size_t *))                              \
    X(int, spvc_resources_get_builtin_resource_list_for_type,                  \
      (H, int, const BuiltinResource **, std::size_t *))                       \
    X(B, spvc_compiler_has_decoration, (H, U, int))                            \
    X(B, spvc_compiler_has_member_decoration, (H, U, U, int))                  \
    X(U, spvc_compiler_get_decoration, (H, U, int))                            \
    X(U, spvc_compiler_get_member_decoration, (H, U, U, int))                  \
    X(void, spvc_compiler_set_decoration, (H, U, int, U))                      \
    X(const char *, spvc_compiler_get_name, (H, U))                            \
    X(const char *, spvc_compiler_get_member_name, (H, U, U))                  \
    X(void, spvc_compiler_set_member_name, (H, U, U, const char *))            \
    X(H, spvc_compiler_get_type_handle, (H, U))                                \
    X(U, spvc_type_get_base_type_id, (H))                                      \
    X(int, spvc_type_get_basetype, (H))                                        \
    X(U, spvc_type_get_bit_width, (H))                                         \
    X(U, spvc_type_get_vector_size, (H))                                       \
    X(U, spvc_type_get_columns, (H))                                           \
    X(U, spvc_type_get_num_array_dimensions, (H))                              \
    X(B, spvc_type_array_dimension_is_literal, (H, U))                         \
    X(U, spvc_type_get_array_dimension, (H, U))                                \
    X(U, spvc_type_get_num_member_types, (H))                                  \
    X(U, spvc_type_get_member_type, (H, U))                                    \
    X(U, spvc_type_get_image_sampled_type, (H))                                \
    X(int, spvc_type_get_image_dimension, (H))                                 \
    X(B, spvc_type_get_image_is_depth, (H))                                    \
    X(B, spvc_type_get_image_arrayed, (H))                                     \
    X(B, spvc_type_get_image_multisampled, (H))                                \
    X(B, spvc_type_get_image_is_storage, (H))                                  \
    X(int, spvc_type_get_image_storage_format, (H))                            \
    X(int, spvc_compiler_get_declared_struct_size, (H, H, std::size_t *))      \
    X(int, spvc_compiler_get_declared_struct_member_size,                      \
      (H, H, U, std::size_t *))                                                \
    X(int, spvc_compiler_type_struct_member_offset, (H, H, U, U *))            \
    X(int, spvc_compiler_type_struct_member_array_stride, (H, H, U, U *))      \
    X(int, spvc_compiler_type_struct_member_matrix_stride, (H, H, U, U *))     \
    X(int, spvc_compiler_build_combined_image_samplers, (H))                   \
    X(int, spvc_compiler_get_combined_image_samplers,                          \
      (H, const Combined **, std::size_t *))
struct Api {
#define EAWR_STAGE_DECLARE(ret, name, args) ret(*name) args {};
    EAWR_STAGE_VKD3D_API(EAWR_STAGE_DECLARE)
    EAWR_STAGE_CROSS_API(EAWR_STAGE_DECLARE)
#undef EAWR_STAGE_DECLARE
};
static_assert(sizeof(U) == 4 && sizeof(int) == 4 && sizeof(B) == 1 &&
              sizeof(Option) == 8);
#if defined(_WIN64)
static_assert(sizeof(Preprocess) == 56 && offsetof(Preprocess, open) == 32 &&
              sizeof(Hlsl) == 48 && offsetof(Hlsl, profile) == 40);
static_assert(sizeof(Compile) == 64 && offsetof(Compile, source) == 16 &&
              offsetof(Compile, source_name) == 56);
static_assert(sizeof(Descriptors) == 32 && sizeof(Pairs) == 32 &&
              sizeof(Signatures) == 64);
static_assert(sizeof(Descriptor) == 28 && sizeof(ResourceBinding) == 32 &&
              sizeof(Interface) == 80);
static_assert(sizeof(Resource) == 24 && sizeof(BuiltinResource) == 32 &&
              sizeof(SignatureElement) == 40);
static_assert(sizeof(Target) == 80 && offsetof(Target, environment) == 24 &&
              offsetof(Target, output_swizzles) == 64);
#endif
} // namespace eawr::presentation::shader_runtime::abi
