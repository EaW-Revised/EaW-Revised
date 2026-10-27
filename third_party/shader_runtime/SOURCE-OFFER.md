# Optional shader compiler corresponding source and replacement

The optional Windows x64 package contains separate, unmodified libraries.
EAWR dynamically resolves their public C interfaces; it does not statically
link vkd3d. Shader translation activation is disabled in this phase.

The exact unmodified vkd3d 2.1 corresponding source is included as
`sources/vkd3d-2.1.tar.xz`, alongside this notice in the distributed package.
Its SHA-256 is
`7510146aff2adfb4ae07ab890701a607e5ff7c66e57100cfc9f630ec92eeda6a`.
The original source is also available at
<https://dl.winehq.org/vkd3d/source/vkd3d-2.1.tar.xz>.
`manifest.json` records the build recipe, compiler and header identities.
There are no project changes to this source; any future implementation changes
must be distributed as a separate LGPL fork with corresponding source.

SPIRV-Cross source for tag `vulkan-sdk-1.3.290.0` is included as
`sources/spirv-cross.tar.gz`, SHA-256
`1333fd2a05ab8a0572106e3b7fb8161ea0b67ab7d0a1b8bdd14b47f89ac8a611`.
It is used under Apache-2.0; the original upstream LICENSE and attribution
are preserved. Upstream has no top-level NOTICE at this pin.

The vkd3d DLL's separate GCC 9.3 SEH runtime and SPIRV-Cross's embedded GCC
13.2 runtime are covered by GPL-3.0-or-later with the GCC Runtime Library
Exception 3.1; the exception permits the independent program combination.
The runtime licence and exception are included. GCC release source is available
at <https://ftp.gnu.org/gnu/gcc/gcc-9.3.0/gcc-9.3.0.tar.xz> and
<https://ftp.gnu.org/gnu/gcc/gcc-13.2.0/gcc-13.2.0.tar.xz>.
The build uses the MinGW-w64 win32 thread model and x64 SEH target recorded in
the manifest. MinGW-w64's applicable runtime notices are preserved as well.

You may replace or modify these separately distributed libraries for your own
use under their licences. Reverse engineering EAWR for debugging modifications
to the LGPL library is permitted. The public C interface is the replacement
boundary; allocations must be released by the same library that created them.
The shipping loader verifies the reviewed identities before loading. To use a
rebuilt replacement, generate and review a replacement lock and identity table,
or explicitly supply that trusted identity table to `Runtime::open`; retain the
public ABI version checks. Package-supplied manifests are never trusted as an
automatic override. The distribution includes no lock that prevents rebuilding
or substituting the library, and requires no signing key or network service.

See `docs/shader-runtime-package.md` in the EAWR source for the offline build,
verification and replacement procedure. No mod files or generated shader text
are part of this dependency package.
