"""CPU-only public C ABI proof, using invented shader input and pinned headers.

Run in a subprocess so module references and OS loader state cannot escape the
probe. No private C++ ABI, CLI subprocess, mod bytes or generated text output.
The independently declared ctypes layouts match the packaged public headers.
"""
from __future__ import annotations

import argparse
import ctypes as c
import os
import struct
from pathlib import Path

from tools.shader_runtime_package.package import verify


class Code(c.Structure):
    _fields_ = [("code", c.c_void_p), ("size", c.c_size_t)]


class CompileInfo(c.Structure):
    _fields_ = [("type", c.c_int), ("next", c.c_void_p), ("source", Code),
                ("source_type", c.c_int), ("target_type", c.c_int), ("options", c.c_void_p),
                ("option_count", c.c_uint), ("log_level", c.c_int), ("source_name", c.c_char_p)]


def bind(dll, name, result, *arguments):
    function = getattr(dll, name)
    function.restype = result
    function.argtypes = list(arguments)
    return function


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def probe(folder: Path):
    require(os.name == "nt" and c.sizeof(c.c_void_p) == 8, "requires qualified Windows x64")
    folder = folder.resolve()
    verify(folder)
    # Dependencies are loaded in reviewed order; no PATH or CWD fallback.
    modules = [c.CDLL(str(folder / name), winmode=0x800) for name in
               ("libgcc_s_seh-1.dll", "libvkd3d-shader-1.dll", "libspirv-cross-c-shared.dll")]
    vkd3d, cross = modules[1:]
    uint_pointer, handle_pointer = c.POINTER(c.c_uint), c.POINTER(c.c_void_p)
    major, minor, patch = c.c_uint(), c.c_uint(), c.c_uint()
    version = bind(vkd3d, "vkd3d_shader_get_version", c.c_char_p, uint_pointer, uint_pointer)
    require(version(c.byref(major), c.byref(minor)) and (major.value, minor.value) == (2, 1), "vkd3d version")
    bind(cross, "spvc_get_version", None, uint_pointer, uint_pointer, uint_pointer)(c.byref(major), c.byref(minor), c.byref(patch))
    require((major.value, minor.value, patch.value) == (0, 61, 0), "SPIRV-Cross C ABI version")
    require(c.sizeof(CompileInfo) == 64 and CompileInfo.source.offset == 16
            and CompileInfo.source_name.offset == 56, "public x64 compile-info layout")
    compile_stage = bind(vkd3d, "vkd3d_shader_compile", c.c_int, c.POINTER(CompileInfo), c.POINTER(Code), handle_pointer)
    free_code = bind(vkd3d, "vkd3d_shader_free_shader_code", None, c.POINTER(Code))
    free_messages = bind(vkd3d, "vkd3d_shader_free_messages", None, c.c_void_p)
    create = bind(cross, "spvc_context_create", c.c_int, handle_pointer)
    destroy = bind(cross, "spvc_context_destroy", None, c.c_void_p)
    release = bind(cross, "spvc_context_release_allocations", None, c.c_void_p)
    error_text = bind(cross, "spvc_context_get_last_error_string", c.c_char_p, c.c_void_p)
    parse = bind(cross, "spvc_context_parse_spirv", c.c_int, c.c_void_p, uint_pointer, c.c_size_t, handle_pointer)
    compiler_create = bind(cross, "spvc_context_create_compiler", c.c_int, c.c_void_p, c.c_int,
                           c.c_void_p, c.c_int, handle_pointer)
    options_create = bind(cross, "spvc_compiler_create_compiler_options", c.c_int, c.c_void_p, handle_pointer)
    options_set = bind(cross, "spvc_compiler_options_set_uint", c.c_int, c.c_void_p, c.c_int, c.c_uint)
    options_install = bind(cross, "spvc_compiler_install_compiler_options", c.c_int, c.c_void_p, c.c_void_p)
    emit = bind(cross, "spvc_compiler_compile", c.c_int, c.c_void_p, handle_pointer)
    # Invented VS1.1: move position and texcoord, with no engine semantics.
    vertex = [0xfffe0101, 0x1f, 0x80000000, 0x900f0000,
              0x01, 0xc00f0000, 0x90e40000, 0x0000ffff]
    copied_source = b""
    for iteration in range(8):
        source = (c.c_uint * len(vertex))(*vertex)
        info = CompileInfo(0, None, Code(c.cast(source, c.c_void_p), c.sizeof(source)), 3, 1,
                           None, 0, 3, b"invented-abi-control")
        output, messages = Code(), c.c_void_p()
        try:
            require(compile_stage(c.byref(info), c.byref(output), c.byref(messages)) == 0, "vkd3d compile")
            binary = c.string_at(output.code, output.size)
            require(len(binary) > 20 and binary[:4] == b"\x03\x02\x23\x07", "SPIR-V output")
        finally:
            free_code(c.byref(output))
            free_messages(messages)
        context = c.c_void_p()
        require(create(c.byref(context)) == 0 and context.value, "context create")
        try:
            invalid, parsed = (c.c_uint * 1)(0), c.c_void_p()
            require(parse(context, invalid, 1, c.byref(parsed)) < 0, "invalid SPIR-V refusal")
            copied_error = bytes(error_text(context) or b"")
            require(copied_error, "context-owned error message")
            release(context)
            words = (c.c_uint * (len(binary) // 4)).from_buffer_copy(binary)
            require(parse(context, words, len(words), c.byref(parsed)) == 0, "SPIR-V parse after release")
            compiler = c.c_void_p()
            # TAKE_OWNERSHIP: do not use parsed IR after this call.
            require(compiler_create(context, 1, parsed, 1, c.byref(compiler)) == 0, "GLSL backend create")
            options = c.c_void_p()
            require(options_create(compiler, c.byref(options)) == 0, "compiler options")
            require(options_set(options, 0x2000000 | 8, 330) == 0, "GLSL version option")
            require(options_install(compiler, options) == 0, "install compiler options")
            text = c.c_void_p()
            require(emit(compiler, c.byref(text)) == 0 and text.value, "shared C API GLSL compile")
            copied_source = c.string_at(text)
            require(b"void main()" in copied_source and b"#version 330" in copied_source, "GLSL result")
            # Child allocations and strings die here; context stays reusable.
            release(context)
        finally:
            destroy(context)
        require(copied_source and copied_error, "caller copies survive context destruction")
        bad = (c.c_uint * 1)(0)
        info.source = Code(c.cast(bad, c.c_void_p), c.sizeof(bad))
        output, messages = Code(), c.c_void_p()
        try:
            require(compile_stage(c.byref(info), c.byref(output), c.byref(messages)) < 0, "invalid bytecode refusal")
            require(messages.value and c.string_at(messages), "vkd3d error allocation")
        finally:
            free_code(c.byref(output))
            free_messages(messages)
    print("public C ABI: versions, 8 compile/create/release/destroy cycles and both error ownership controls passed")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("package", type=Path)
    probe(parser.parse_args().package)
