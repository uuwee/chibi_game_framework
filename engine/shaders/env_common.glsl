// Environment preprocessing: turns an equirectangular radiance texture into
// cube maps. Uses its own pipeline layout (env_pipeline_layout in
// engine/renderer_internal.h): the cube map level being written, then the
// material textures.
#extension GL_EXT_scalar_block_layout : require
#extension GL_EXT_nonuniform_qualifier : require

#define ENV_CUBE_SIZE 256u
#define ENV_CUBE_MIPS 7u
#define ENV_IRRADIANCE_SIZE 32u
#define PI 3.14159265358979

layout(local_size_x = 8, local_size_y = 8) in;

layout(push_constant, scalar) uniform PushConstants {
  uint render;
  uint source; // texture index of the equirectangular image
  uint mip;
  uint pad;
} pc;

layout(set = 0, binding = 0, rgba16f) uniform writeonly image2DArray target;
layout(set = 1, binding = 0) uniform sampler2D textures[];

// Direction through a texel center of a cube face, in the face orientation
// that samplerCube uses.
vec3 cube_direction(uint face, vec2 uv) {
  vec2 st = uv * 2.0 - 1.0;
  float s = st.x;
  float t = st.y;
  vec3 d;
  switch (face) {
  case 0u: d = vec3(1.0, -t, -s); break;
  case 1u: d = vec3(-1.0, -t, s); break;
  case 2u: d = vec3(s, 1.0, t); break;
  case 3u: d = vec3(s, -1.0, -t); break;
  case 4u: d = vec3(s, -t, 1.0); break;
  default: d = vec3(-s, -t, -1.0); break;
  }
  return normalize(d);
}

// Radiance of the source image in a direction. The top row of the image is
// straight up.
vec3 sample_source(vec3 d, float lod) {
  float u = atan(d.x, -d.z) / (2.0 * PI) + 0.5;
  float v = acos(clamp(d.y, -1.0, 1.0)) / PI;
  return textureLod(textures[nonuniformEXT(pc.source)], vec2(u, v), lod).rgb;
}

// Solid angle of one texel of the source image's first level.
float source_texel_solid_angle() {
  vec2 size = vec2(textureSize(textures[nonuniformEXT(pc.source)], 0));
  return 4.0 * PI / (size.x * size.y);
}

vec2 hammersley(uint i, uint count) {
  uint bits = bitfieldReverse(i);
  return vec2(float(i) / float(count), float(bits) * 2.3283064365386963e-10);
}

// Orthonormal basis around n.
mat3 tangent_frame(vec3 n) {
  vec3 up = abs(n.y) < 0.999 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
  vec3 t = normalize(cross(up, n));
  vec3 b = cross(n, t);
  return mat3(t, b, n);
}
