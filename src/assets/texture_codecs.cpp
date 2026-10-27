// Keep the dependency implementation separate from the project decoder glue.
// Only the two qualified additional image formats and memory input are enabled.
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_NO_STDIO
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>
