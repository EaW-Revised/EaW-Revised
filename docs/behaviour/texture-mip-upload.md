# Texture mip upload

| Rule | Presentation contract | Evidence |
|---|---|---|
| MT-01 | A valid multi-level DDS chain may stop before 1x1. Preserve the decoded asset and every authored level; complete only the upload copy through 1x1 because Godot's image API accepts a full-chain boolean rather than an explicit level count. Single-level images remain unmipped. | Partial-chain upload failure (legacy EAWR-4638): debug-build texture creation requests default mip levels; the referenced texture API defines a complete chain. Synthetic renderer contracts and graphical DXT1/DXT3/DXT5/RGBA uploads cover seven authored levels of a 1024-square image. |

Missing uncompressed levels use a channel-wise box average, including odd edges
and one-pixel axes. Missing compressed levels replicate the nearest source block
on a halved block grid, retaining its encoding and selectors. This is an explicit
approximation for the absent levels, rather than a claim of identical retail
filtering. Existing compressed levels remain byte-identical and compressed,
avoiding a full-image expansion or recompression during loading. The same rule
applies to mesh and normalized particle uploads. GPU accounting includes the
generated tail, including when texture pressure drops authored top levels.

The DDS decoder's accepted formats, validation and partial mip counts do not
change. The generated tail never becomes part of the decoded asset or content
identity. No mod names, file names or retail mip counts drive this adaptation.
