#!/usr/bin/env bash
set -euo pipefail

# Reproducible x86_64-Linux -> AArch64-Linux build. Run from WSL/Linux.
# Keep parallelism deliberately low because Godot and godot-cpp are memory-heavy.
repo_root="$(git rev-parse --show-toplevel)"
out_root="${EAWR_GODOT_ARM_OUT:-${repo_root}/out/godot-arm-package}"
godot_source="${EAWR_GODOT_SOURCE:-${repo_root}/out/godot/deps/godot-engine}"
toolchain_root="${EAWR_ARM_GNU_ROOT:-${repo_root}/out/prototypes/sdl_wgpu/deps/arm-gnu-toolchain-14.2.rel1-x86_64-aarch64-none-linux-gnu}"
python_bin="${EAWR_PYTHON:-${HOME}/.local/bin/python3.12}"
scons_pythonpath="${EAWR_SCONS_PYTHONPATH:-$(python3 -c 'import os,SCons; print(os.path.dirname(os.path.dirname(SCons.__file__)))')}"
jobs="${EAWR_ARM_JOBS:-2}"
triple="aarch64-none-linux-gnu"
suffix="eawr_arm_cross"
template_source="${godot_source}/bin/godot.linuxbsd.template_release.arm64.${suffix}"
template_copy="${out_root}/artifacts/godot.linuxbsd.template_release.arm64"
extension_source="${repo_root}/prototypes/godot/project/bin/libeawr_godot.linux.template_release.arm64.so"
host_extension_source="${repo_root}/prototypes/godot/project/bin/libeawr_godot.linux.template_release.x86_64.so"
editor_zip="${repo_root}/out/godot/deps/Godot_v4.7.2-stable_linux.x86_64.zip"
editor_bin="${out_root}/editor/Godot_v4.7.2-stable_linux.x86_64"
stage="${out_root}/project"
package="${out_root}/package"

out_root="$(realpath -m "${out_root}")"
allowed_out_root="$(realpath -m "${repo_root}/out/godot-arm-package")"
case "${out_root}" in
    "${allowed_out_root}"|"${allowed_out_root}"/*) ;;
    *) echo "EAWR_GODOT_ARM_OUT must remain inside ${allowed_out_root}" >&2; exit 2 ;;
esac
template_copy="${out_root}/artifacts/godot.linuxbsd.template_release.arm64"
editor_bin="${out_root}/editor/Godot_v4.7.2-stable_linux.x86_64"
stage="${out_root}/project"
package="${out_root}/package"

case "${jobs}" in
    ''|*[!0-9]*) echo "EAWR_ARM_JOBS must be an integer" >&2; exit 2 ;;
esac
if (( jobs < 1 || jobs > 4 )); then
    echo "EAWR_ARM_JOBS must be between 1 and 4 (default 2)" >&2
    exit 2
fi

for required in "${python_bin}" "${editor_zip}" "${toolchain_root}/bin/${triple}-gcc" \
    "${toolchain_root}/bin/${triple}-g++" "${toolchain_root}/bin/${triple}-readelf" \
    "${host_extension_source}"; do
    if [[ ! -f "${required}" ]]; then
        echo "Missing required cached dependency: ${required}" >&2
        exit 2
    fi
done

if [[ "$(git -C "${godot_source}" rev-parse HEAD)" != "ed1daf0bf001b61586d9930840f2f1394092c079" ]]; then
    echo "Godot source is not the pinned 4.7.2-stable commit" >&2
    exit 2
fi
if [[ "$(git -C "${repo_root}/out/godot/deps/godot-cpp" rev-parse HEAD)" != "507ed9d840c01a3c5b2a39af8bb4000bfac30bf5" ]]; then
    echo "godot-cpp source is not the pinned 10.0.0-stable commit" >&2
    exit 2
fi
for checkout in "${godot_source}" "${repo_root}/out/godot/deps/godot-cpp"; do
    if ! git -C "${checkout}" diff --quiet || ! git -C "${checkout}" diff --cached --quiet; then
        echo "Pinned dependency checkout has tracked modifications: ${checkout}" >&2
        exit 2
    fi
done
if [[ "$("${toolchain_root}/bin/${triple}-gcc" -dumpmachine)" != "${triple}" ]] || \
   [[ "$("${toolchain_root}/bin/${triple}-gcc" -dumpfullversion)" != "14.2.1" ]]; then
    echo "Arm compiler identity does not match aarch64-none-linux-gnu GCC 14.2.1 (14.2.Rel1)" >&2
    exit 2
fi

expected_editor_sha512="$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1], encoding="utf-8"))["godot"]["linux_x86_64_zip_sha512"])' "${repo_root}/prototypes/godot/dependencies.json")"
actual_editor_sha512="$(sha512sum "${editor_zip}" | awk '{print $1}')"
if [[ "${actual_editor_sha512}" != "${expected_editor_sha512}" ]]; then
    echo "Cached official Godot editor archive failed the pinned SHA-512 check" >&2
    exit 2
fi

mkdir -p "${out_root}/artifacts" "${out_root}/binutils" "${out_root}/scons-cache" "${package}"
for tool in ar as ld nm objcopy objdump ranlib readelf strip gcc-ar gcc-nm gcc-ranlib; do
    target="${toolchain_root}/bin/${triple}-${tool}"
    if [[ -x "${target}" ]]; then
        ln -sfn "${target}" "${out_root}/binutils/${tool}"
    fi
done
export PATH="${out_root}/binutils:${toolchain_root}/bin:${PATH}"

echo "Building pinned Godot ARM64 release template with ${jobs} job(s)..."
(
    cd "${godot_source}"
    PYTHONPATH="${scons_pythonpath}${PYTHONPATH:+:${PYTHONPATH}}" "${python_bin}" -m SCons \
        platform=linuxbsd target=template_release arch=arm64 production=yes \
        use_llvm=no linker=bfd extra_suffix="${suffix}" cache_path="${out_root}/scons-cache" \
        CC="${toolchain_root}/bin/${triple}-gcc" \
        CXX="${toolchain_root}/bin/${triple}-g++" \
        LINK="${toolchain_root}/bin/${triple}-g++" \
        -j"${jobs}"
)
cp -f "${template_source}" "${template_copy}"

echo "Building the matching ARM64 GDExtension..."
export EAWR_ARM_GNU_ROOT="${toolchain_root}"
cmake -S "${repo_root}/prototypes/godot" -B "${out_root}/extension-build" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="${repo_root}/prototypes/godot/cmake/aarch64-none-linux-gnu.cmake" \
    -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF
cmake --build "${out_root}/extension-build" --parallel "${jobs}" --target eawr_godot

echo "Staging only public project resources and the ARM64 extension..."
rm -rf "${stage}" "${package}"
mkdir -p "${stage}/bin" "${stage}/common" "${package}" "${out_root}/editor"
cp "${repo_root}/prototypes/godot/project/project.godot" "${stage}/"
cp "${repo_root}/prototypes/godot/project/main.tscn" "${stage}/"
cp "${repo_root}/prototypes/godot/project/eawr_godot.gdextension" "${stage}/"
cp "${repo_root}/prototypes/common/scene.json" "${stage}/common/"
cp "${repo_root}/tests/replay/fixtures/original-v1.eawr-replay" "${stage}/common/"
cp "${extension_source}" "${stage}/bin/"
# The x86_64 editor scans and instantiates the project before exporting. Give
# that host process its matching extension; the ARM export preset selects only
# the ARM library, and the strict package verifier rejects any leaked host ELF.
cp "${host_extension_source}" "${stage}/bin/"

cat > "${stage}/export_presets.cfg" <<EOF
[preset.0]

name="Linux ARM64"
platform="Linux"
runnable=false
advanced_options=true
dedicated_server=false
custom_features=""
export_filter="all_resources"
include_filter="common/*.json,common/*.eawr-replay"
exclude_filter=""
export_path="${package}/eawr-godot.arm64"
script_export_mode=2

[preset.0.options]

custom_template/debug=""
custom_template/release="${template_copy}"
binary_format/architecture="arm64"
texture_format/s3tc_bptc=true
texture_format/etc2_astc=false
EOF

if [[ ! -x "${editor_bin}" ]]; then
    python3 -c 'import sys,zipfile; zipfile.ZipFile(sys.argv[1]).extractall(sys.argv[2])' "${editor_zip}" "${out_root}/editor"
    chmod +x "${editor_bin}"
fi

echo "Exporting with the pinned official 4.7.2 editor and custom ARM64 template..."
"${editor_bin}" --headless --path "${stage}" --export-release "Linux ARM64" "${package}/eawr-godot.arm64"

python3 "${repo_root}/prototypes/godot/tools/verify_arm64_package.py" \
    --template "${template_copy}" \
    --extension "${package}/libeawr_godot.linux.template_release.arm64.so" \
    --executable "${package}/eawr-godot.arm64" \
    --pck "${package}/eawr-godot.pck" \
    --readelf "${toolchain_root}/bin/${triple}-readelf" \
    --sysroot "${toolchain_root}/${triple}/libc" \
    --manifest "${out_root}/verification.json"

echo "ARM64 package complete: ${package}"
echo "Cross-compiled only; native ARM64 execution and GPU qualification remain open."
