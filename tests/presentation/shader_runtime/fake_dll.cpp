// Independently invented public-ABI boundary double. No compiler implementation.
extern "C" {
__declspec(dllexport) const char* vkd3d_shader_get_version(unsigned int* major, unsigned int* minor) {
#ifdef VKD3D_BAD
    *major = 99;
    *minor = 0;
#else
    *major = 2;
    *minor = 1;
#endif
    return "invented mismatch";
}
__declspec(dllexport) void spvc_get_version(unsigned int* major, unsigned int* minor, unsigned int* patch) {
    *major = 99;
    *minor = 0;
    *patch = 0;
}
#ifndef MISSING_EXPORT
__declspec(dllexport) void vkd3d_shader_compile() {}
#endif
__declspec(dllexport) void vkd3d_shader_free_shader_code() {}
__declspec(dllexport) void vkd3d_shader_free_messages() {}
__declspec(dllexport) void vkd3d_shader_preprocess() {}
__declspec(dllexport) void spvc_context_create() {}
__declspec(dllexport) void spvc_context_destroy() {}
__declspec(dllexport) void spvc_context_release_allocations() {}
__declspec(dllexport) void spvc_context_get_last_error_string() {}
__declspec(dllexport) void spvc_context_parse_spirv() {}
__declspec(dllexport) void spvc_context_create_compiler() {}
__declspec(dllexport) void spvc_compiler_create_compiler_options() {}
__declspec(dllexport) void spvc_compiler_options_set_uint() {}
__declspec(dllexport) void spvc_compiler_install_compiler_options() {}
__declspec(dllexport) void spvc_compiler_compile() {}
__declspec(dllexport) void spvc_compiler_create_shader_resources() {}
__declspec(dllexport) void spvc_resources_get_resource_list_for_type() {}
}
