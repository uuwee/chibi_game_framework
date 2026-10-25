#ifndef CGF_ENGINE_LOADER_INTERNAL_H
#define CGF_ENGINE_LOADER_INTERNAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "include/renderer.h"

/* A decoded image ready for texture_create. */
typedef struct LoadedImage {
  CGF_TextureFormat format;
  uint32_t width;
  uint32_t height;
  uint32_t mip_count;
  uint8_t *data;
  size_t size;
  bool has_alpha; /* the format stores an alpha channel */
} LoadedImage;

/* Loads a DDS file. Color formats use their sRGB variant when srgb is set.
   Two-channel (BC5) images without mips get a full mip chain. Writes a
   reason to error on failure. */
bool image_load_dds(const char *path, bool srgb, LoadedImage *out, char *error,
                    size_t error_size);

/* Loads a Radiance HDR file as RGBA16_FLOAT with a full mip chain. */
bool image_load_hdr(const char *path, LoadedImage *out, char *error,
                    size_t error_size);

void image_free(LoadedImage *image);

#endif
