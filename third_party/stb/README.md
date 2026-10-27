# stb_image

Vendored `stb_image` v2.30 from https://github.com/nothings/stb at commit
`013ac3beddff3dbffafd5177e7972067cd2b5083`.

The unmodified header's SHA-256 is
`594c2fe35d49488b4382dbfaec8f98366defca819d916ac95becf3e75f4200b3`.
The adjacent LICENSE preserves the upstream MIT/public-domain licensing options.

The portable asset library enables only PNG/JPEG decoding from memory. Existing
BMP/DDS/TGA readers retain their contracts. The project wrapper checks the texture
pixel budget before decoding and uses no global decoder settings.
