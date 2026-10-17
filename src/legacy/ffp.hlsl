// The fixed-function pipeline of Direct3D 7 to 9 as one vertex and one pixel
// shader, driven by constants: no shader compilation while a game runs. The
// legacy device fills the constant buffers from the render, texture stage and
// transform states; branches on them are uniform across a draw.

// Matrices are given in Direct3D's row-major layout, for mul(vector, matrix).
#pragma pack_matrix(row_major)

#define MAX_STAGES 8
#define MAX_LIGHTS 8

// ---- Vertex stage ----

struct Light {
  float4 diffuse;
  float4 specular;
  float4 ambient;
  float4 position;   // view space
  float4 direction;  // view space, normalized
  float4 params0;    // range, falloff, attenuation 0, attenuation 1
  float4 params1;    // attenuation 2, cos(theta / 2), cos(phi / 2), type (1 point, 2 spot, 3 directional)
};

// Changes with every object: a buffer of its own, written alone.
cbuffer FfpTransforms : register(b3) {
  float4x4 worldView[4];     // world matrix i * view
  float4x4 normalMatrix[4];  // inverse transpose of worldView (3x3)
};

cbuffer FfpVertex : register(b0) {
  float4x4 projection;
  float4x4 textureMatrix[MAX_STAGES];
  float4   rhwScale;         // pre-transformed vertices: window to clip space
  float4   rhwOffset;
  float4   materialDiffuse;
  float4   materialAmbient;
  float4   materialSpecular;
  float4   materialEmissive;
  float4   globalAmbient;
  float4   fogVertex;        // power, fog start, fog end, fog density
  uint4    vertexFlags;      // transformed, lighting, blend weights | indexed << 8, light count
  uint4    colorSources;     // diffuse, ambient, specular, emissive: 0 material, 1 color 0, 2 color 1
  uint4    vertexFlags2;     // specular, local viewer, normalize normals, vertex fog mode | range fog << 4
  uint4    texCoordSetup[MAX_STAGES];  // source set, generation, transform count, input size
  Light    lights[MAX_LIGHTS];
  float4   clipPlanes[6];      // user clip planes in view space
  uint4    clipFlags;          // enabled planes
};

struct VSInput {
  float4 position : POSITION;
  float4 weights  : BLENDWEIGHT;
  uint4  indices  : BLENDINDICES;
  float3 normal   : NORMAL;
  float4 color0   : COLOR0;
  float4 color1   : COLOR1;
  float4 tex[8]   : TEXCOORD0;
};

// The varying slots of Direct3D 9 shaders (d3d9_shader.h), so translated
// shaders and these link either way.
struct VSOutput {
  float4 position : SV_Position;
  float4 fog      : FOG0;       // vertex fog factor, eye depth for table fog
  float4 color0   : COLOR0;
  float4 color1   : COLOR1;
  float4 tex[8]   : TEXCOORD0;
  float4 clip0    : SV_ClipDistance0;
  float2 clip1    : SV_ClipDistance1;
};

// What the pixel stage reads: the varyings without the clip distances.
struct PSInput {
  float4 position : SV_Position;
  float4 fog      : FOG0;
  float4 color0   : COLOR0;
  float4 color1   : COLOR1;
  float4 tex[8]   : TEXCOORD0;
};

float4 expandTexCoord(float4 value, uint size) {
  // Direct3D 9 puts a 1 after the last given component.
  if (size == 1) return float4(value.x, 1.0f, 0.0f, 0.0f);
  if (size == 2) return float4(value.xy, 1.0f, 0.0f);
  if (size == 3) return float4(value.xyz, 1.0f);
  return value;
}

float4 sourceColor(uint source, float4 material, float4 color0, float4 color1) {
  if (source == 1) return color0;
  if (source == 2) return color1;
  return material;
}

VSOutput vs_main(VSInput input) {
  VSOutput output;
  const bool transformed = vertexFlags.x != 0;
  float3 viewPosition = 0.0f;
  float3 viewNormal = float3(0.0f, 0.0f, 0.0f);

  if (transformed) {
    // Window coordinates and 1/w: back to clip space, perspective intact.
    const float w = input.position.w != 0.0f ? 1.0f / input.position.w : 1.0f;
    const float3 ndc = input.position.xyz * rhwScale.xyz + rhwOffset.xyz;
    output.position = float4(ndc * w, w);
    viewPosition = float3(0.0f, 0.0f, w);
  } else {
    const uint blendCount = vertexFlags.z & 0xffu;
    const bool indexed = (vertexFlags.z >> 8) != 0;
    const float4 position = float4(input.position.xyz, 1.0f);
    if (blendCount == 0) {
      viewPosition = mul(position, worldView[0]).xyz;
      viewNormal = mul(input.normal, (float3x3)normalMatrix[0]);
    } else {
      float remaining = 1.0f;
      [loop]
      for (uint i = 0; i <= blendCount && i < 4; i++) {
        const float weight = i < blendCount ? input.weights[i] : remaining;
        remaining -= weight;
        const uint matrixIndex = indexed ? (input.indices[i] & 3u) : i;
        viewPosition += weight * mul(position, worldView[matrixIndex]).xyz;
        viewNormal += weight * mul(input.normal, (float3x3)normalMatrix[matrixIndex]);
      }
    }
    if (vertexFlags2.z != 0 && dot(viewNormal, viewNormal) > 0.0f)
      viewNormal = normalize(viewNormal);
    output.position = mul(float4(viewPosition, 1.0f), projection);
  }

  // Colors: vertex colors, or lit.
  float4 color0 = input.color0;
  float4 color1 = input.color1;
  if (!transformed && vertexFlags.y != 0) {
    const float4 diffuseColor  = sourceColor(colorSources.x, materialDiffuse, input.color0, input.color1);
    const float4 ambientColor  = sourceColor(colorSources.y, materialAmbient, input.color0, input.color1);
    const float4 specularColor = sourceColor(colorSources.z, materialSpecular, input.color0, input.color1);
    const float4 emissiveColor = sourceColor(colorSources.w, materialEmissive, input.color0, input.color1);
    float3 diffuse = 0.0f, specular = 0.0f, ambient = 0.0f;
    const float3 toViewer = vertexFlags2.y != 0 ? normalize(-viewPosition) : float3(0.0f, 0.0f, -1.0f);
    [loop]
    for (uint i = 0; i < vertexFlags.w && i < MAX_LIGHTS; i++) {
      const Light light = lights[i];
      const uint type = (uint)light.params1.w;
      float3 toLight;
      float attenuation = 1.0f;
      if (type == 3) {
        toLight = -light.direction.xyz;
      } else {
        const float3 offset = light.position.xyz - viewPosition;
        const float distance = length(offset);
        if (distance > light.params0.x)
          continue;
        toLight = distance > 0.0f ? offset / distance : float3(0.0f, 0.0f, 0.0f);
        const float denominator = light.params0.z + light.params0.w * distance + light.params1.x * distance * distance;
        attenuation = denominator > 0.0f ? 1.0f / denominator : 1.0f;
        if (type == 2) {
          const float rho = dot(-toLight, light.direction.xyz);
          if (rho <= light.params1.z)
            attenuation = 0.0f;
          else if (rho < light.params1.y)
            attenuation *= pow(saturate((rho - light.params1.z) / (light.params1.y - light.params1.z)), light.params0.y);
        }
      }
      ambient += attenuation * light.ambient.rgb;
      const float lambert = dot(viewNormal, toLight);
      if (lambert > 0.0f) {
        diffuse += attenuation * lambert * light.diffuse.rgb;
        if (vertexFlags2.x != 0) {
          const float3 halfway = normalize(toLight + toViewer);
          specular += attenuation * pow(saturate(dot(viewNormal, halfway)), fogVertex.x) * light.specular.rgb;
        }
      }
    }
    color0.rgb = saturate(emissiveColor.rgb + ambientColor.rgb * (globalAmbient.rgb + ambient) + diffuseColor.rgb * diffuse);
    color0.a = diffuseColor.a;
    color1.rgb = vertexFlags2.x != 0 ? saturate(specularColor.rgb * specular) : float3(0.0f, 0.0f, 0.0f);
    color1.a = 0.0f;
  }
  output.color0 = color0;
  output.color1 = color1;

  // Fog: from the specular alpha when no mode computes it.
  const uint fogMode = vertexFlags2.w & 0xfu;
  const float fogDistance = (vertexFlags2.w >> 4) != 0 ? length(viewPosition) : abs(viewPosition.z);
  float fog = input.color1.a;
  if (!transformed && fogMode == 3)
    fog = saturate((fogVertex.z - fogDistance) / max(fogVertex.z - fogVertex.y, 1e-6f));
  else if (!transformed && fogMode == 1)
    fog = saturate(exp(-fogDistance * fogVertex.w));
  else if (!transformed && fogMode == 2)
    fog = saturate(exp(-(fogDistance * fogVertex.w) * (fogDistance * fogVertex.w)));
  // The factor, and the distances the pixel stage computes fog from (as
  // NVIDIA's Direct3D 9 does for vertex fog too).
  output.fog = float4(fog, abs(viewPosition.z), length(viewPosition), 0.0f);

  // Texture coordinates per stage: generated or from a set, transformed.
  [unroll]
  for (uint stage = 0; stage < MAX_STAGES; stage++) {
    const uint4 setup = texCoordSetup[stage];
    float4 tc;
    if (setup.y == 1)
      tc = float4(viewNormal, 1.0f);
    else if (setup.y == 2)
      tc = float4(viewPosition, 1.0f);
    else if (setup.y == 3)
      tc = float4(reflect(normalize(viewPosition), viewNormal), 1.0f);
    else if (setup.y == 4) {
      const float3 r = reflect(normalize(viewPosition), viewNormal);
      const float m = 2.0f * sqrt(r.x * r.x + r.y * r.y + (r.z + 1.0f) * (r.z + 1.0f));
      tc = float4(r.x / m + 0.5f, r.y / m + 0.5f, 0.0f, 1.0f);
    } else {
      float4 raw = input.tex[0];
      [unroll] for (uint s = 1; s < 8; s++) { if (setup.x == s) raw = input.tex[s]; }
      tc = expandTexCoord(raw, setup.w);
    }
    if (setup.z != 0)
      tc = mul(tc, textureMatrix[stage]);
    output.tex[stage] = tc;
  }

  // User clip planes (world space, given here in view space).
  float clip[6];
  [unroll]
  for (uint plane = 0; plane < 6; plane++)
    clip[plane] = !transformed && ((clipFlags.x >> plane) & 1u) ? dot(float4(viewPosition, 1.0f), clipPlanes[plane]) : 1.0f;
  output.clip0 = float4(clip[0], clip[1], clip[2], clip[3]);
  output.clip1 = float2(clip[4], clip[5]);
  return output;
}

// ---- Pixel stage ----

Texture2D    textures2d[MAX_STAGES]   : register(t0);
TextureCube  texturesCube[MAX_STAGES] : register(t8);
Texture3D    textures3d[MAX_STAGES]   : register(t16);
SamplerState samplers[MAX_STAGES]     : register(s0);

cbuffer FfpPixel : register(b1) {
  uint4  stageOps[MAX_STAGES];   // color op, alpha op, result (0 current, 1 temp), texture type | projected << 4 | count << 8 | ATI2 (blue 1) << 12
  uint4  stageArgs[MAX_STAGES];  // color arg 1, 2, 0, alpha args 1 | 2 << 8 | 0 << 16
  float4 stageConstant[MAX_STAGES];
  float4 bumpMatrix[MAX_STAGES];
  float4 bumpLuminance[MAX_STAGES];
  float4 textureFactor;
  float4 fogColor;
  float4 fogTable;               // start, end, density, mode per pixel (0 none, 1 exp, 2 exp2, 3 linear)
  float4 alphaTest;              // reference, function, enabled, specular enabled
  uint4  pixelFlags;             // fog enabled, stage count, range fog
};

float4 argument(uint arg, float4 diffuse, float4 current, float4 texel, float4 specular, float4 temp, float4 constant) {
  float4 value;
  switch (arg & 0xfu) {
    case 0:  value = diffuse; break;
    case 1:  value = current; break;
    case 2:  value = texel; break;
    case 3:  value = textureFactor; break;
    case 4:  value = specular; break;
    case 5:  value = temp; break;
    default: value = constant; break;
  }
  if (arg & 0x10u) value = 1.0f - value;
  if (arg & 0x20u) value = value.aaaa;
  return value;
}

float4 combine(uint op, float4 a1, float4 a2, float4 a0, float4 diffuse, float4 texel, float4 current) {
  switch (op) {
    case 2:  return a1;
    case 3:  return a2;
    case 4:  return a1 * a2;
    case 5:  return 2.0f * a1 * a2;
    case 6:  return 4.0f * a1 * a2;
    case 7:  return a1 + a2;
    case 8:  return a1 + a2 - 0.5f;
    case 9:  return 2.0f * (a1 + a2 - 0.5f);
    case 10: return a1 - a2;
    case 11: return a1 + a2 - a1 * a2;
    case 12: return lerp(a2, a1, diffuse.a);
    case 13: return lerp(a2, a1, texel.a);
    case 14: return lerp(a2, a1, textureFactor.a);
    case 15: return a1 + a2 * (1.0f - texel.a);
    case 16: return lerp(a2, a1, current.a);
    case 18: return float4(a1.rgb + a1.a * a2.rgb, a1.a);
    case 19: return float4(a1.rgb * a2.rgb, a1.a + a2.a);
    case 20: return float4(a1.rgb + (1.0f - a1.a) * a2.rgb, a1.a);
    case 21: return float4((1.0f - a1.rgb) * a2.rgb, a1.a + a2.a);
    case 24: return saturate(4.0f * dot(a1.rgb - 0.5f, a2.rgb - 0.5f)).xxxx;
    case 25: return a0 + a1 * a2;
    case 26: return lerp(a2, a1, a0);
    default: return a1 * a2;
  }
}

bool alphaPasses(float alpha) {
  const float reference = alphaTest.x;
  switch ((uint)alphaTest.y) {
    case 1:  return false;
    case 2:  return alpha < reference;
    case 3:  return alpha == reference;
    case 4:  return alpha <= reference;
    case 5:  return alpha > reference;
    case 6:  return alpha != reference;
    case 7:  return alpha >= reference;
    default: return true;
  }
}

float4 ps_main(PSInput input) : SV_Target {
  const float4 diffuse = input.color0;
  const float4 specular = input.color1;
  float4 current = diffuse;
  float4 temp = 0.0f;
  float2 bumpOffset = 0.0f;
  float bumpScale = 1.0f;

  [unroll]
  for (uint stage = 0; stage < MAX_STAGES; stage++) {
    const uint4 ops = stageOps[stage];
    if (stage >= pixelFlags.y || ops.x == 1)
      break;
    const uint textureType = ops.w & 0xfu;
    float4 tc = input.tex[stage];
    tc.xy += bumpOffset;
    if ((ops.w >> 4) & 1u) {
      const uint count = (ops.w >> 8) & 0xfu;
      const float q = count == 4 ? tc.w : (count == 3 ? tc.z : tc.y);
      if (q != 0.0f) tc.xyz /= q;
    }
    float4 texel = 1.0f;
    if (textureType == 1)
      texel = textures2d[stage].Sample(samplers[stage], tc.xy);
    else if (textureType == 2)
      texel = texturesCube[stage].Sample(samplers[stage], tc.xyz);
    else if (textureType == 3)
      texel = textures3d[stage].Sample(samplers[stage], tc.xyz);
    // ATI2 reads (green, red, 1, 1); BC5 gives blue 0.
    if ((ops.w >> 12) & 1u)
      texel.b = 1.0f;
    texel.rgb *= bumpScale;
    bumpOffset = 0.0f;
    bumpScale = 1.0f;

    // Bump mapping perturbs the next stage's coordinates.
    if (ops.x == 22 || ops.x == 23) {
      const float4 m = bumpMatrix[stage];
      bumpOffset = float2(m.x * texel.r + m.z * texel.g, m.y * texel.r + m.w * texel.g);
      if (ops.x == 23)
        bumpScale = saturate(texel.b * bumpLuminance[stage].x + bumpLuminance[stage].y);
      continue;
    }

    const uint4 args = stageArgs[stage];
    const float4 constant = stageConstant[stage];
    const float4 c1 = argument(args.x, diffuse, current, texel, specular, temp, constant);
    const float4 c2 = argument(args.y, diffuse, current, texel, specular, temp, constant);
    const float4 c0 = argument(args.z, diffuse, current, texel, specular, temp, constant);
    const float4 color = combine(ops.x, c1, c2, c0, diffuse, texel, current);
    float alpha;
    if (ops.x == 24) {
      alpha = color.a;
    } else if (ops.y == 1) {
      alpha = current.a;
    } else {
      const float4 a1 = argument(args.w & 0xffu, diffuse, current, texel, specular, temp, constant);
      const float4 a2 = argument((args.w >> 8) & 0xffu, diffuse, current, texel, specular, temp, constant);
      const float4 a0 = argument((args.w >> 16) & 0xffu, diffuse, current, texel, specular, temp, constant);
      alpha = combine(ops.y, a1, a2, a0, diffuse, texel, current).a;
    }
    const float4 result = saturate(float4(color.rgb, alpha));
    if (ops.z == 1)
      temp = result;
    else
      current = result;
  }

  if (alphaTest.w != 0.0f)
    current.rgb = saturate(current.rgb + specular.rgb);

  if (pixelFlags.x != 0) {
    float fog = input.fog.x;
    const uint tableMode = (uint)fogTable.w;
    const float depth = pixelFlags.z != 0 ? input.fog.z : input.fog.y;
    if (tableMode == 3)
      fog = saturate((fogTable.y - depth) / max(fogTable.y - fogTable.x, 1e-6f));
    else if (tableMode == 1)
      fog = saturate(exp(-depth * fogTable.z));
    else if (tableMode == 2)
      fog = saturate(exp(-(depth * fogTable.z) * (depth * fogTable.z)));
    current.rgb = lerp(fogColor.rgb, current.rgb, fog);
  }

  if (alphaTest.z != 0.0f && !alphaPasses(current.a))
    discard;
  return current;
}

// ---- Clears of parts of a target, and blits ----

struct QuadOutput {
  float4 position : SV_Position;
  float2 tex      : TEXCOORD0;
};

cbuffer QuadConstants : register(b2) {
  float4 quadRect;      // clip space x0, y0, x1, y1
  float4 quadTexRect;   // texture u0, v0, u1, v1
  float4 quadColor;
  float4 quadDepth;     // depth, color key enabled, color key (packed), unused
};

QuadOutput quad_vs(uint id : SV_VertexID) {
  QuadOutput output;
  const float2 corner = float2(id & 1u, id >> 1);
  output.position = float4(lerp(quadRect.xy, quadRect.zw, corner), quadDepth.x, 1.0f);
  output.tex = lerp(quadTexRect.xy, quadTexRect.zw, corner);
  return output;
}

float4 clear_ps(QuadOutput input) : SV_Target {
  return quadColor;
}

Texture2D    blitSource  : register(t0);
SamplerState blitSampler : register(s0);

float4 blit_ps(QuadOutput input) : SV_Target {
  const float4 texel = blitSource.Sample(blitSampler, input.tex);
  // Source color key: texels of the key color are not drawn.
  if (quadDepth.y != 0.0f && all(abs(texel.rgb - quadColor.rgb) < 0.5f / 255.0f))
    discard;
  return texel;
}

// Presentation through a gamma ramp (SetGammaRamp): 256 entries per
// channel, interpolated between like a display's lookup table.
Texture2D gammaRamp : register(t1);

float4 gamma_ps(QuadOutput input) : SV_Target {
  const float4 texel = blitSource.Sample(blitSampler, input.tex);
  const float3 coord = saturate(texel.rgb) * (255.0f / 256.0f) + 0.5f / 256.0f;
  return float4(gammaRamp.Sample(blitSampler, float2(coord.r, 0.5f)).r,
                gammaRamp.Sample(blitSampler, float2(coord.g, 0.5f)).g,
                gammaRamp.Sample(blitSampler, float2(coord.b, 0.5f)).b, texel.a);
}
