// Shading: GGX metal/roughness BRDF, directional shadows, environment
// lookups and the screen probes of the global illumination.

float distribution_ggx(float n_dot_h, float alpha) {
  float alpha2 = alpha * alpha;
  float d = n_dot_h * n_dot_h * (alpha2 - 1.0) + 1.0;
  return alpha2 / (PI * d * d);
}

// Height-correlated Smith visibility, including 1 / (4 n.l n.v).
float visibility_smith(float n_dot_v, float n_dot_l, float alpha) {
  float alpha2 = alpha * alpha;
  float v = n_dot_l * sqrt(n_dot_v * n_dot_v * (1.0 - alpha2) + alpha2);
  float l = n_dot_v * sqrt(n_dot_l * n_dot_l * (1.0 - alpha2) + alpha2);
  return 0.5 / max(v + l, 1e-6);
}

vec3 specular_color(vec3 base_color, float metallic) {
  return mix(vec3(0.04), base_color, metallic);
}

// Outgoing radiance toward v per unit of incoming radiance from l, times
// n.l.
vec3 surface_brdf(Surface s, vec3 v, vec3 l) {
  vec3 n = s.normal;
  float n_dot_l = dot(n, l);
  if (n_dot_l <= 0.0)
    return vec3(0.0);
  float n_dot_v = max(dot(n, v), 1e-4);
  vec3 h = normalize(v + l);
  float n_dot_h = max(dot(n, h), 0.0);
  float v_dot_h = max(dot(v, h), 0.0);
  float alpha = max(s.roughness * s.roughness, 0.002);
  vec3 f0 = specular_color(s.base_color, s.metallic);
  vec3 fresnel = f0 + (1.0 - f0) * pow(1.0 - v_dot_h, 5.0);
  vec3 specular = distribution_ggx(n_dot_h, alpha) *
                  visibility_smith(n_dot_v, n_dot_l, alpha) * fresnel;
  vec3 diffuse = s.base_color * (1.0 - s.metallic) * (1.0 - fresnel) / PI;
  return (diffuse + specular) * n_dot_l;
}

// Split-sum environment BRDF, analytic fit (Karis): scale and bias of f0.
vec2 environment_brdf(float roughness, float n_dot_v) {
  const vec4 c0 = vec4(-1.0, -0.0275, -0.572, 0.022);
  const vec4 c1 = vec4(1.0, 0.0425, 1.04, -0.04);
  vec4 r = roughness * c0 + c1;
  float a004 = min(r.x * r.x, exp2(-9.28 * n_dot_v)) * r.x + r.y;
  return vec2(-1.04, 1.04) * a004 + r.zw;
}

// Direction in the environment texture's own frame.
vec3 environment_direction(vec3 d) {
  float c = render_data.environment.y;
  float s = render_data.environment.z;
  return vec3(c * d.x - s * d.z, d.y, s * d.x + c * d.z);
}

// Radiance arriving from a direction, prefiltered for a roughness of
// lod / (ENV_CUBE_MIPS - 1).
vec3 environment_radiance(vec3 direction, float lod) {
  uint slot = render_data.environment_slot;
  if (slot == ENVIRONMENT_NONE)
    return render_data.sky.rgb;
  vec3 radiance =
      textureLod(env_radiance[slot], environment_direction(direction), lod)
          .rgb *
      render_data.environment.x;
  return min(radiance, vec3(HALF_MAX));
}

vec3 environment_irradiance(vec3 normal) {
  uint slot = render_data.environment_slot;
  if (slot == ENVIRONMENT_NONE)
    return render_data.sky.rgb * PI;
  return textureLod(env_irradiance[slot], environment_direction(normal), 0.0)
             .rgb *
         render_data.environment.x;
}

float shadow_cascade(uint cascade, vec3 position, vec3 normal) {
  float texel = render_data.shadow_texel[cascade];
  vec3 offset_position = position + normal * texel * 1.5;
  vec4 clip = render_data.shadow_view_projection[cascade] *
              vec4(offset_position, 1.0);
  vec2 uv = clip.xy * 0.5 + 0.5;
  if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0))))
    return 1.0;
  float depth = clip.z - 0.5 / float(SHADOW_MAP_SIZE);
  vec2 step = vec2(1.0 / float(SHADOW_MAP_SIZE));
  float sum = 0.0;
  for (int y = -1; y <= 1; y++) {
    for (int x = -1; x <= 1; x++)
      sum += textureGrad(shadow_map,
                         vec4(uv + vec2(x, y) * step, float(cascade), depth),
                         vec2(0.0), vec2(0.0));
  }
  return sum / 9.0;
}

// Visibility of the scene's main directional light, blending neighboring
// cascades near their boundaries.
float directional_shadow(vec3 position, vec3 normal) {
  float depth = world_view_depth(depth_rows(), position);
  vec4 splits = render_data.shadow_splits;
  for (uint c = 0u; c < SHADOW_CASCADE_COUNT; c++) {
    if (depth >= splits[c])
      continue;
    float shadow = shadow_cascade(c, position, normal);
    float begin = c == 0u ? 0.0 : splits[c - 1u];
    float band = (splits[c] - begin) * 0.15;
    float t = clamp((depth - (splits[c] - band)) / band, 0.0, 1.0);
    if (t > 0.0) {
      float next = c + 1u < SHADOW_CASCADE_COUNT
                       ? shadow_cascade(c + 1u, position, normal)
                       : 1.0;
      shadow = mix(shadow, next, t);
    }
    return shadow;
  }
  return 1.0;
}

// Direct lighting of a surface by the render's lights.
vec3 direct_lighting(Surface s, vec3 position, vec3 v, uvec2 pixel) {
  vec3 color = vec3(0.0);
  uint first = render_data.light_offset;
  for (uint i = 0u; i < render_data.directional_count; i++) {
    Light light = lights[first + i];
    vec3 l = light.position_radius.xyz;
    vec3 brdf = surface_brdf(s, v, l);
    if (all(equal(brdf, vec3(0.0))))
      continue;
    float shadow = 1.0;
    if (i == render_data.shadow_light)
      shadow = directional_shadow(position, s.geometric_normal);
    color += light.color.rgb * brdf * shadow;
  }

  uvec2 tile = pixel / TILE_SIZE;
  uint base = (tile.y * render_data.tile_count.x + tile.x) *
              (MAX_LIGHTS_PER_TILE + 1u);
  uint count = tile_lights[base];
  uint points = first + render_data.directional_count;
  for (uint i = 0u; i < count; i++) {
    Light light = lights[points + tile_lights[base + 1u + i]];
    vec3 to_light = light.position_radius.xyz - position;
    float distance_squared = dot(to_light, to_light);
    float radius = light.position_radius.w;
    if (distance_squared >= radius * radius)
      continue;
    vec3 l = to_light * inversesqrt(distance_squared);
    // Inverse-square falloff, windowed to reach zero at the radius.
    float ratio = distance_squared / (radius * radius);
    float window = clamp(1.0 - ratio * ratio, 0.0, 1.0);
    float attenuation = window * window / max(distance_squared, 0.01);
    color += light.color.rgb * attenuation * surface_brdf(s, v, l);
  }
  return color;
}

// Interpolates the filtered L1 spherical harmonics of the four cascade 0
// probes around a pixel. With plane weights, probes off the pixel's surface
// plane count less. Returns false when no probe is near a surface.
bool interpolate_probes(vec2 pixel, vec3 position, vec3 normal,
                        bool plane_weights, out vec4 sh[4]) {
  for (int i = 0; i < 4; i++)
    sh[i] = vec4(0.0);
  uvec2 screen = render_data.screen_size;
  uint spacing = GI_PROBE_SPACING;
  uvec2 probe_count = (screen + spacing - 1u) / spacing;
  uint filtered = probe_count.x * probe_count.y; // after the raw probes
  vec2 coordinate = pixel / float(spacing) - 0.5;
  ivec2 base = ivec2(floor(coordinate));
  vec2 f = coordinate - vec2(base);
  float scale = distance(position, render_data.camera_position.xyz);

  float weight_sum = 0.0;
  for (int y = 0; y < 2; y++) {
    for (int x = 0; x < 2; x++) {
      ivec2 q = clamp(base + ivec2(x, y), ivec2(0), ivec2(probe_count) - 1);
      Probe probe = probes[filtered + uint(q.y) * probe_count.x + uint(q.x)];
      if (probe.position.w == 0.0)
        continue;
      float weight = (x == 0 ? 1.0 - f.x : f.x) * (y == 0 ? 1.0 - f.y : f.y);
      if (plane_weights) {
        float plane_distance =
            abs(dot(probe.position.xyz - position, normal)) / scale;
        weight *= exp(-plane_distance * 40.0);
      }
      weight = max(weight, 1e-5);
      sh[0] += probe.sh[0] * weight;
      for (int i = 1; i < 4; i++)
        sh[i].rgb += probe.sh[i].rgb * weight;
      weight_sum += weight;
    }
  }
  if (weight_sum <= 0.0)
    return false;
  for (int i = 0; i < 4; i++)
    sh[i] /= weight_sum;
  return true;
}

// Irradiance from L1 radiance: pi * L0 * Y0 + 2 pi / 3 * L1 * Y1(n).
vec3 sh_irradiance(vec4 sh[4], vec3 n) {
  vec3 e = PI * 0.282095 * sh[0].rgb +
           2.0 * PI / 3.0 * 0.488603 *
               (sh[1].rgb * n.y + sh[2].rgb * n.z + sh[3].rgb * n.x);
  return max(e, vec3(0.0));
}

vec3 sh_radiance(vec4 sh[4], vec3 d) {
  vec3 l = 0.282095 * sh[0].rgb +
           0.488603 * (sh[1].rgb * d.y + sh[2].rgb * d.z + sh[3].rgb * d.x);
  return max(l, vec3(0.0));
}

// Indirect light reflected toward v: diffuse from the probes (or the
// environment), specular from the environment where the probes see the sky
// and from the probes' low-frequency radiance elsewhere.
void indirect_lighting(Surface s, vec2 pixel, vec3 position, vec3 v,
                       bool plane_weights, out vec3 diffuse,
                       out vec3 specular) {
  vec3 n = s.normal;
  vec4 sh[4];
  bool have_probes = interpolate_probes(pixel, position, n, plane_weights, sh);
  float n_dot_v = max(dot(n, v), 1e-4);
  vec3 r = reflect(-v, n);
  vec3 sky_specular =
      environment_radiance(r, s.roughness * float(ENV_CUBE_MIPS - 1u));
  vec3 irradiance;
  vec3 reflected;
  if (have_probes) {
    irradiance = sh_irradiance(sh, n);
    float sky_visibility = clamp(sh[0].w, 0.0, 1.0);
    reflected = mix(sh_radiance(sh, r), sky_specular, sky_visibility);
  } else {
    irradiance = environment_irradiance(n);
    reflected = sky_specular;
  }
  vec2 ab = environment_brdf(s.roughness, n_dot_v);
  vec3 specular_weight =
      specular_color(s.base_color, s.metallic) * ab.x + ab.y;
  // Specular occlusion from the ambient occlusion (Lagarde).
  float occlusion = s.occlusion;
  float specular_occlusion = clamp(
      pow(n_dot_v + occlusion, exp2(-16.0 * s.roughness - 1.0)) - 1.0 +
          occlusion,
      0.0, 1.0);
  diffuse = s.base_color * (1.0 - s.metallic) * (1.0 - specular_weight) / PI *
            irradiance * occlusion;
  specular = reflected * specular_weight * specular_occlusion;
}
