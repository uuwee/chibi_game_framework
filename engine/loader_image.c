#include "loader_internal.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool fail(char *error, size_t error_size, const char *format, ...) {
  va_list args;
  va_start(args, format);
  vsnprintf(error, error_size, format, args);
  va_end(args);
  return false;
}

static uint8_t *read_file(const char *path, size_t *out_size) {
  FILE *file = fopen(path, "rb");
  if (!file)
    return NULL;
  uint8_t *data = NULL;
  if (_fseeki64(file, 0, SEEK_END) == 0) {
    long long size = _ftelli64(file);
    if (size >= 0 && _fseeki64(file, 0, SEEK_SET) == 0) {
      data = malloc(size > 0 ? (size_t)size : 1);
      if (data && fread(data, 1, (size_t)size, file) != (size_t)size) {
        free(data);
        data = NULL;
      }
      *out_size = (size_t)size;
    }
  }
  fclose(file);
  return data;
}

static uint32_t read_u32(const uint8_t *p) {
  return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
         (uint32_t)p[3] << 24;
}

static uint32_t mip_dimension(uint32_t size, uint32_t mip) {
  return size >> mip ? size >> mip : 1;
}

static uint32_t full_mip_count(uint32_t width, uint32_t height) {
  uint32_t size = width > height ? width : height;
  uint32_t count = 1;
  while (size > 1) {
    size >>= 1;
    count++;
  }
  return count;
}

void image_free(LoadedImage *image) {
  free(image->data);
  *image = (LoadedImage){0};
}

///////////////////////////////////////////////////////////////////////////////
// BC4 / BC5 mip generation
///////////////////////////////////////////////////////////////////////////////

static void bc4_decode(const uint8_t block[8], uint8_t out[16]) {
  uint8_t r0 = block[0], r1 = block[1];
  uint8_t palette[8] = {r0, r1};
  if (r0 > r1) {
    for (int i = 1; i < 7; i++)
      palette[i + 1] = (uint8_t)(((7 - i) * r0 + i * r1 + 3) / 7);
  } else {
    for (int i = 1; i < 5; i++)
      palette[i + 1] = (uint8_t)(((5 - i) * r0 + i * r1 + 2) / 5);
    palette[6] = 0;
    palette[7] = 255;
  }
  uint64_t bits = 0;
  for (int i = 0; i < 6; i++)
    bits |= (uint64_t)block[2 + i] << (8 * i);
  for (int i = 0; i < 16; i++)
    out[i] = palette[(bits >> (3 * i)) & 7];
}

/* Eight-value mode between the block's extremes. */
static void bc4_encode(const uint8_t values[16], uint8_t block[8]) {
  uint8_t lo = 255, hi = 0;
  for (int i = 0; i < 16; i++) {
    lo = values[i] < lo ? values[i] : lo;
    hi = values[i] > hi ? values[i] : hi;
  }
  block[0] = hi;
  block[1] = lo;
  uint64_t bits = 0;
  if (hi > lo) {
    for (int i = 0; i < 16; i++) {
      /* k / 7 is the weight of hi; palette index 0 is hi, 1 is lo, then
         from (6 hi + lo) / 7 down to (hi + 6 lo) / 7. */
      int k = (int)lrintf((float)(values[i] - lo) * 7.0f / (float)(hi - lo));
      uint64_t index = k == 7 ? 0 : k == 0 ? 1 : (uint64_t)(8 - k);
      bits |= index << (3 * i);
    }
  }
  for (int i = 0; i < 6; i++)
    block[2 + i] = (uint8_t)(bits >> (8 * i));
}

/* Decodes a BC4 or BC5 level into one byte per channel per pixel. */
static void bc_decode_level(const uint8_t *blocks, uint32_t width,
                            uint32_t height, uint32_t channels,
                            uint8_t *pixels) {
  uint32_t blocks_x = (width + 3) / 4;
  uint32_t blocks_y = (height + 3) / 4;
  for (uint32_t by = 0; by < blocks_y; by++) {
    for (uint32_t bx = 0; bx < blocks_x; bx++) {
      const uint8_t *block = blocks + (by * blocks_x + bx) * 8 * channels;
      for (uint32_t c = 0; c < channels; c++) {
        uint8_t values[16];
        bc4_decode(block + 8 * c, values);
        for (uint32_t i = 0; i < 16; i++) {
          uint32_t x = bx * 4 + (i & 3), y = by * 4 + (i >> 2);
          if (x < width && y < height)
            pixels[(y * width + x) * channels + c] = values[i];
        }
      }
    }
  }
}

static void bc_encode_level(const uint8_t *pixels, uint32_t width,
                            uint32_t height, uint32_t channels,
                            uint8_t *blocks) {
  uint32_t blocks_x = (width + 3) / 4;
  uint32_t blocks_y = (height + 3) / 4;
  for (uint32_t by = 0; by < blocks_y; by++) {
    for (uint32_t bx = 0; bx < blocks_x; bx++) {
      uint8_t *block = blocks + (by * blocks_x + bx) * 8 * channels;
      for (uint32_t c = 0; c < channels; c++) {
        uint8_t values[16];
        for (uint32_t i = 0; i < 16; i++) {
          /* Pixels past the edge repeat the edge. */
          uint32_t x = bx * 4 + (i & 3), y = by * 4 + (i >> 2);
          x = x < width ? x : width - 1;
          y = y < height ? y : height - 1;
          values[i] = pixels[(y * width + x) * channels + c];
        }
        bc4_encode(values, block + 8 * c);
      }
    }
  }
}

/* Replaces a single-level BC4 / BC5 image with one that has all mips, each
   the 2x2 box average of the previous one. */
static bool generate_bc_mips(LoadedImage *image, uint32_t channels) {
  uint32_t mip_count = full_mip_count(image->width, image->height);
  size_t size = 0;
  for (uint32_t mip = 0; mip < mip_count; mip++) {
    uint32_t w = mip_dimension(image->width, mip);
    uint32_t h = mip_dimension(image->height, mip);
    size += (size_t)((w + 3) / 4) * ((h + 3) / 4) * 8 * channels;
  }
  uint8_t *data = malloc(size);
  uint8_t *pixels =
      malloc((size_t)image->width * image->height * channels);
  uint8_t *smaller = malloc((size_t)image->width * image->height * channels);
  if (!data || !pixels || !smaller) {
    free(data);
    free(pixels);
    free(smaller);
    return false;
  }
  memcpy(data, image->data, image->size);
  bc_decode_level(image->data, image->width, image->height, channels, pixels);
  size_t offset = image->size;
  uint32_t w = image->width, h = image->height;
  for (uint32_t mip = 1; mip < mip_count; mip++) {
    uint32_t nw = mip_dimension(image->width, mip);
    uint32_t nh = mip_dimension(image->height, mip);
    for (uint32_t y = 0; y < nh; y++) {
      for (uint32_t x = 0; x < nw; x++) {
        for (uint32_t c = 0; c < channels; c++) {
          uint32_t sum = 0;
          for (uint32_t i = 0; i < 4; i++) {
            uint32_t sx = x * 2 + (i & 1), sy = y * 2 + (i >> 1);
            sx = sx < w ? sx : w - 1;
            sy = sy < h ? sy : h - 1;
            sum += pixels[(sy * w + sx) * channels + c];
          }
          smaller[(y * nw + x) * channels + c] = (uint8_t)((sum + 2) / 4);
        }
      }
    }
    bc_encode_level(smaller, nw, nh, channels, data + offset);
    offset += (size_t)((nw + 3) / 4) * ((nh + 3) / 4) * 8 * channels;
    uint8_t *swap = pixels;
    pixels = smaller;
    smaller = swap;
    w = nw;
    h = nh;
  }
  free(pixels);
  free(smaller);
  free(image->data);
  image->data = data;
  image->size = size;
  image->mip_count = mip_count;
  return true;
}

///////////////////////////////////////////////////////////////////////////////
// DDS
///////////////////////////////////////////////////////////////////////////////

#define FOURCC(a, b, c, d)                                                     \
  ((uint32_t)(a) | (uint32_t)(b) << 8 | (uint32_t)(c) << 16 |                  \
   (uint32_t)(d) << 24)

#define DDSD_MIPMAPCOUNT 0x20000u
#define DDPF_FOURCC 0x4u
#define DDPF_RGB 0x40u

typedef struct DdsFormat {
  CGF_TextureFormat format;
  CGF_TextureFormat srgb_format;
  uint32_t block_bytes; /* per 4x4 block, or per pixel when uncompressed */
  bool compressed;
  bool has_alpha;
  bool swap_red_blue; /* stored as BGRA */
} DdsFormat;

static bool dds_format(const uint8_t *header, size_t file_size,
                       DdsFormat *out, size_t *data_offset) {
  uint32_t flags = read_u32(header + 80);
  uint32_t fourcc = read_u32(header + 84);
  *data_offset = 128;
  if (flags & DDPF_FOURCC) {
    switch (fourcc) {
    case FOURCC('D', 'X', 'T', '1'):
      *out = (DdsFormat){CGF_TEXTURE_FORMAT_BC1_UNORM,
                         CGF_TEXTURE_FORMAT_BC1_SRGB, 8, true, false, false};
      return true;
    case FOURCC('D', 'X', 'T', '5'):
      *out = (DdsFormat){CGF_TEXTURE_FORMAT_BC3_UNORM,
                         CGF_TEXTURE_FORMAT_BC3_SRGB, 16, true, true, false};
      return true;
    case FOURCC('A', 'T', 'I', '1'):
    case FOURCC('B', 'C', '4', 'U'):
      *out = (DdsFormat){CGF_TEXTURE_FORMAT_BC4_UNORM,
                         CGF_TEXTURE_FORMAT_BC4_UNORM, 8, true, false, false};
      return true;
    case FOURCC('A', 'T', 'I', '2'):
    case FOURCC('B', 'C', '5', 'U'):
      *out = (DdsFormat){CGF_TEXTURE_FORMAT_BC5_UNORM,
                         CGF_TEXTURE_FORMAT_BC5_UNORM, 16, true, false, false};
      return true;
    case FOURCC('D', 'X', '1', '0'): {
      if (file_size < 148)
        return false;
      *data_offset = 148;
      switch (read_u32(header + 128)) { /* DXGI_FORMAT */
      case 71:
      case 72:
        *out = (DdsFormat){CGF_TEXTURE_FORMAT_BC1_UNORM,
                           CGF_TEXTURE_FORMAT_BC1_SRGB, 8, true, false, false};
        return true;
      case 77:
      case 78:
        *out = (DdsFormat){CGF_TEXTURE_FORMAT_BC3_UNORM,
                           CGF_TEXTURE_FORMAT_BC3_SRGB, 16, true, true, false};
        return true;
      case 80:
        *out = (DdsFormat){CGF_TEXTURE_FORMAT_BC4_UNORM,
                           CGF_TEXTURE_FORMAT_BC4_UNORM, 8, true, false, false};
        return true;
      case 83:
        *out = (DdsFormat){CGF_TEXTURE_FORMAT_BC5_UNORM,
                           CGF_TEXTURE_FORMAT_BC5_UNORM, 16, true, false, false};
        return true;
      case 98:
      case 99:
        *out = (DdsFormat){CGF_TEXTURE_FORMAT_BC7_UNORM,
                           CGF_TEXTURE_FORMAT_BC7_SRGB, 16, true, true, false};
        return true;
      case 28:
      case 29:
        *out = (DdsFormat){CGF_TEXTURE_FORMAT_RGBA8_UNORM,
                           CGF_TEXTURE_FORMAT_RGBA8_SRGB, 4, false, true, false};
        return true;
      case 87:
      case 91:
        *out = (DdsFormat){CGF_TEXTURE_FORMAT_RGBA8_UNORM,
                           CGF_TEXTURE_FORMAT_RGBA8_SRGB, 4, false, true, true};
        return true;
      case 10:
        *out = (DdsFormat){CGF_TEXTURE_FORMAT_RGBA16_FLOAT,
                           CGF_TEXTURE_FORMAT_RGBA16_FLOAT, 8, false, true,
                           false};
        return true;
      case 2:
        *out = (DdsFormat){CGF_TEXTURE_FORMAT_RGBA32_FLOAT,
                           CGF_TEXTURE_FORMAT_RGBA32_FLOAT, 16, false, true,
                           false};
        return true;
      default:
        return false;
      }
    }
    default:
      return false;
    }
  }
  /* Uncompressed 32-bit RGBA or BGRA. */
  if ((flags & DDPF_RGB) && read_u32(header + 88) == 32) {
    bool bgra = read_u32(header + 92) == 0x00ff0000u;
    *out = (DdsFormat){CGF_TEXTURE_FORMAT_RGBA8_UNORM,
                       CGF_TEXTURE_FORMAT_RGBA8_SRGB, 4, false, true, bgra};
    return true;
  }
  return false;
}

bool image_load_dds(const char *path, bool srgb, LoadedImage *out, char *error,
                    size_t error_size) {
  *out = (LoadedImage){0};
  size_t file_size = 0;
  uint8_t *file = read_file(path, &file_size);
  if (!file)
    return fail(error, error_size, "cannot read %s", path);
  if (file_size < 128 || read_u32(file) != FOURCC('D', 'D', 'S', ' ')) {
    free(file);
    return fail(error, error_size, "%s is not a DDS file", path);
  }
  DdsFormat format;
  size_t data_offset;
  if (!dds_format(file, file_size, &format, &data_offset)) {
    free(file);
    return fail(error, error_size, "%s has an unsupported pixel format", path);
  }
  uint32_t height = read_u32(file + 12);
  uint32_t width = read_u32(file + 16);
  uint32_t mip_count = (read_u32(file + 8) & DDSD_MIPMAPCOUNT)
                           ? read_u32(file + 28)
                           : 1;
  if (width == 0 || height == 0) {
    free(file);
    return fail(error, error_size, "%s has no pixels", path);
  }
  uint32_t full = full_mip_count(width, height);
  mip_count = mip_count == 0 ? 1 : mip_count > full ? full : mip_count;

  /* Keep the mips present in the file. */
  size_t size = 0;
  uint32_t available = 0;
  for (uint32_t mip = 0; mip < mip_count; mip++) {
    uint32_t w = mip_dimension(width, mip), h = mip_dimension(height, mip);
    size_t mip_size = format.compressed
                          ? (size_t)((w + 3) / 4) * ((h + 3) / 4) *
                                format.block_bytes
                          : (size_t)w * h * format.block_bytes;
    if (data_offset + size + mip_size > file_size)
      break;
    size += mip_size;
    available++;
  }
  if (available == 0) {
    free(file);
    return fail(error, error_size, "%s is truncated", path);
  }

  uint8_t *data = malloc(size);
  if (!data) {
    free(file);
    return fail(error, error_size, "out of memory reading %s", path);
  }
  memcpy(data, file + data_offset, size);
  free(file);
  if (format.swap_red_blue) {
    for (size_t i = 0; i + 3 < size; i += 4) {
      uint8_t red = data[i + 2];
      data[i + 2] = data[i];
      data[i] = red;
    }
  }
  *out = (LoadedImage){
      .format = srgb ? format.srgb_format : format.format,
      .width = width,
      .height = height,
      .mip_count = available,
      .data = data,
      .size = size,
      .has_alpha = format.has_alpha,
  };
  bool two_channels = format.format == CGF_TEXTURE_FORMAT_BC5_UNORM;
  bool one_channel = format.format == CGF_TEXTURE_FORMAT_BC4_UNORM;
  if (available == 1 && full > 1 && (two_channels || one_channel) &&
      !generate_bc_mips(out, two_channels ? 2 : 1)) {
    image_free(out);
    return fail(error, error_size, "out of memory reading %s", path);
  }
  return true;
}

///////////////////////////////////////////////////////////////////////////////
// Radiance HDR
///////////////////////////////////////////////////////////////////////////////

static uint16_t float_to_half(float value) {
  uint32_t bits;
  memcpy(&bits, &value, sizeof(bits));
  uint32_t sign = (bits >> 16) & 0x8000u;
  if (!(value == value))
    return (uint16_t)(sign | 0x7e00u);
  float magnitude = fabsf(value);
  if (magnitude > 65504.0f)
    return (uint16_t)(sign | 0x7bffu);
  if (magnitude < 6.103515625e-05f) /* subnormal */
    return (uint16_t)(sign | (uint32_t)lrintf(magnitude * 16777216.0f));
  memcpy(&bits, &magnitude, sizeof(bits));
  uint32_t exponent = (bits >> 23) - 127 + 15;
  uint32_t mantissa = bits & 0x7fffffu;
  uint32_t half = (exponent << 10) | (mantissa >> 13);
  /* Round to nearest even. */
  uint32_t rest = mantissa & 0x1fffu;
  if (rest > 0x1000u || (rest == 0x1000u && (half & 1u)))
    half++;
  return (uint16_t)(sign | half);
}

/* Reads one scanline of RGBE pixels, in either the flat or the run-length
   encoded form. Returns the new read position, or NULL on error. */
static const uint8_t *read_scanline(const uint8_t *p, const uint8_t *end,
                                    uint32_t width, uint8_t *rgbe) {
  if (width >= 8 && width < 32768 && end - p >= 4 && p[0] == 2 && p[1] == 2 &&
      !(p[2] & 0x80)) {
    if (((uint32_t)p[2] << 8 | p[3]) != width)
      return NULL;
    p += 4;
    for (uint32_t channel = 0; channel < 4; channel++) {
      uint32_t x = 0;
      while (x < width) {
        if (p >= end)
          return NULL;
        uint32_t count = *p++;
        if (count > 128) {
          count -= 128;
          if (p >= end || x + count > width)
            return NULL;
          for (uint32_t i = 0; i < count; i++)
            rgbe[(x + i) * 4 + channel] = *p;
          p++;
        } else {
          if (count == 0 || x + count > width || (size_t)(end - p) < count)
            return NULL;
          for (uint32_t i = 0; i < count; i++)
            rgbe[(x + i) * 4 + channel] = *p++;
        }
        x += count;
      }
    }
    return p;
  }
  if ((size_t)(end - p) < (size_t)width * 4)
    return NULL;
  memcpy(rgbe, p, (size_t)width * 4);
  return p + (size_t)width * 4;
}

bool image_load_hdr(const char *path, LoadedImage *out, char *error,
                    size_t error_size) {
  *out = (LoadedImage){0};
  size_t file_size = 0;
  uint8_t *file = read_file(path, &file_size);
  if (!file)
    return fail(error, error_size, "cannot read %s", path);
  const uint8_t *p = file;
  const uint8_t *end = file + file_size;
  if (file_size < 11 || (memcmp(file, "#?RADIANCE", 10) != 0 &&
                         memcmp(file, "#?RGBE", 6) != 0)) {
    free(file);
    return fail(error, error_size, "%s is not a Radiance HDR file", path);
  }
  /* Header lines end with an empty line, then the resolution line. */
  bool header_done = false;
  while (p < end && !header_done) {
    const uint8_t *line = p;
    while (p < end && *p != '\n')
      p++;
    header_done = p - line == 0;
    p++;
  }
  char resolution[64] = {0};
  size_t length = 0;
  while (p < end && *p != '\n' && length + 1 < sizeof(resolution))
    resolution[length++] = (char)*p++;
  p++;
  /* Only the common top-to-bottom, left-to-right layout: "-Y h +X w". */
  long width = 0, height = 0;
  if (strncmp(resolution, "-Y ", 3) == 0) {
    char *rest = NULL;
    height = strtol(resolution + 3, &rest, 10);
    if (rest && strncmp(rest, " +X ", 4) == 0)
      width = strtol(rest + 4, NULL, 10);
  }
  if (width <= 0 || height <= 0 || width > 16384 || height > 16384) {
    free(file);
    return fail(error, error_size, "%s has an unsupported layout", path);
  }

  float *pixels = malloc((size_t)width * height * 4 * sizeof(float));
  uint8_t *rgbe = malloc((size_t)width * 4);
  bool ok = pixels && rgbe;
  for (int y = 0; ok && y < height; y++) {
    p = read_scanline(p, end, (uint32_t)width, rgbe);
    ok = p != NULL;
    for (int x = 0; ok && x < width; x++) {
      const uint8_t *e = rgbe + x * 4;
      float scale = e[3] ? ldexpf(1.0f, (int)e[3] - 136) : 0.0f;
      float *out_pixel = pixels + ((size_t)y * width + x) * 4;
      out_pixel[0] = e[0] * scale;
      out_pixel[1] = e[1] * scale;
      out_pixel[2] = e[2] * scale;
      out_pixel[3] = 1.0f;
    }
  }
  free(rgbe);
  free(file);
  if (!ok) {
    free(pixels);
    return fail(error, error_size, "%s is truncated or corrupt", path);
  }

  /* Mips by 2x2 averaging, stored as half floats. */
  uint32_t mip_count = full_mip_count((uint32_t)width, (uint32_t)height);
  size_t texels = 0;
  for (uint32_t mip = 0; mip < mip_count; mip++)
    texels += (size_t)mip_dimension((uint32_t)width, mip) *
              mip_dimension((uint32_t)height, mip);
  uint16_t *data = malloc(texels * 4 * sizeof(uint16_t));
  float *level = pixels;
  float *next = malloc((size_t)width * height * sizeof(float));
  if (!data || !next) {
    free(data);
    free(next);
    free(pixels);
    return fail(error, error_size, "out of memory reading %s", path);
  }
  size_t offset = 0;
  uint32_t w = (uint32_t)width, h = (uint32_t)height;
  for (uint32_t mip = 0; mip < mip_count; mip++) {
    for (size_t i = 0; i < (size_t)w * h * 4; i++)
      data[offset + i] = float_to_half(level[i]);
    offset += (size_t)w * h * 4;
    if (mip + 1 == mip_count)
      break;
    uint32_t nw = w > 1 ? w / 2 : 1, nh = h > 1 ? h / 2 : 1;
    for (uint32_t y = 0; y < nh; y++) {
      for (uint32_t x = 0; x < nw; x++) {
        for (uint32_t c = 0; c < 4; c++) {
          float sum = 0.0f;
          for (uint32_t i = 0; i < 4; i++) {
            uint32_t sx = x * 2 + (i & 1), sy = y * 2 + (i >> 1);
            sx = sx < w ? sx : w - 1;
            sy = sy < h ? sy : h - 1;
            sum += level[((size_t)sy * w + sx) * 4 + c];
          }
          next[((size_t)y * nw + x) * 4 + c] = sum * 0.25f;
        }
      }
    }
    float *swap = level;
    level = next;
    next = swap;
    w = nw;
    h = nh;
  }
  free(level);
  free(next);
  *out = (LoadedImage){
      .format = CGF_TEXTURE_FORMAT_RGBA16_FLOAT,
      .width = (uint32_t)width,
      .height = (uint32_t)height,
      .mip_count = mip_count,
      .data = (uint8_t *)data,
      .size = texels * 4 * sizeof(uint16_t),
  };
  return true;
}
