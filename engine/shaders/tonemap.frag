#version 460
// Maps the HDR image to the window. Colors are premultiplied by coverage, so
// the window keeps its previous contents where nothing was drawn.
#include "common.glsl"

layout(constant_id = 0) const bool ENCODE_SRGB = false;

layout(location = 0) out vec4 out_color;

vec3 aces(vec3 x) {
  // Narkowicz's fit of the ACES filmic curve.
  return clamp((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14), 0.0,
               1.0);
}

vec3 encode_srgb(vec3 c) {
  return mix(c * 12.92, 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055,
             step(vec3(0.0031308), c));
}

vec3 heat(float t) {
  return clamp(vec3(1.5 - abs(4.0 * t - 3.0), 1.5 - abs(4.0 * t - 2.0),
                    1.5 - abs(4.0 * t - 1.0)),
               0.0, 1.0);
}

void main() {
  vec4 hdr = imageLoad(hdr_image, ivec2(gl_FragCoord.xy));
  vec3 color = aces(hdr.rgb);
  if (pc.render.debug_mode == DEBUG_MODE_LIGHTS && hdr.a > 0.0) {
    uvec2 tile = uvec2(gl_FragCoord.xy) / TILE_SIZE;
    uint base = (tile.y * pc.render.tile_count.x + tile.x) *
                (MAX_LIGHTS_PER_TILE + 1u);
    float count = float(pc.render.tile_lights.data[base]);
    color = mix(color, heat(count / 16.0), 0.6);
  }
  if (ENCODE_SRGB)
    color = encode_srgb(color);
  out_color = vec4(color, hdr.a);
}
