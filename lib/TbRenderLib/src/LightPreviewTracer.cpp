/*
 Copyright (C) 2026 Kristian Duske

 This file is part of TrenchBroom.

 TrenchBroom is free software: you can redistribute it and/or modify
 it under the terms of the GNU General Public License as published by
 the Free Software Foundation, either version 3 of the License, or
 (at your option) any later version.

 TrenchBroom is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU General Public License for more details.

 You should have received a copy of the GNU General Public License
 along with TrenchBroom. If not, see <http://www.gnu.org/licenses/>.
 */

#include "render/LightPreviewTracer.h"

#include "gl/Camera.h"
#include "render/LightPreviewScene.h"

#include "vm/constants.h"
#include "vm/scalar.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace tb::render
{
namespace
{

/**
 * The distance the reciprocal of which the compilers use as the unit of attenuation.
 * A light with the default "wait" of 1 fades over roughly this many units per unit of its
 * "light" value.
 */
constexpr auto AttenuationScale = 128.0f;

/**
 * How many lights one shading point may consider before it starts picking among them.
 *
 * NOTE: a point that more than this many lights reach ignores the surplus, which would
 * show up as a room that previews darker than it compiles. No map seen so far comes
 * close, but if one does, the fix is a reservoir here rather than a larger array.
 */
constexpr auto MaxLightCandidates = size_t(256);

constexpr auto MaxRayDistance = 1.0e7f;

/**
 * How many see-through surfaces one path may cross. A stack of water brushes must not be
 * able to keep a path going forever, and past a few layers there is nothing left to see.
 */
constexpr auto MaxPassThroughs = 8;

/** Below this, what is left of a path is too dim to be worth following. */
constexpr auto MinThroughput = 1.0e-3f;

/**
 * A small, fast generator. The preview needs a different sequence for every pixel and
 * every pass, and it needs it without touching shared state, so the stream is derived
 * from the pixel and pass numbers rather than carried between them.
 */
class Rng
{
private:
  uint32_t m_state;

public:
  explicit Rng(const uint32_t seed)
    : m_state{seed | 1u}
  {
    next();
  }

  uint32_t nextBits()
  {
    m_state ^= m_state << 13;
    m_state ^= m_state >> 17;
    m_state ^= m_state << 5;
    return m_state;
  }

  /** A number in [0, 1). */
  float next() { return float(nextBits() >> 8) * (1.0f / 16777216.0f); }
};

uint32_t hashSeed(const uint32_t a, const uint32_t b)
{
  auto h = a * 0x9e3779b9u ^ (b + 0x85ebca6bu + (a << 6) + (a >> 2));
  h ^= h >> 16;
  h *= 0x7feb352du;
  h ^= h >> 15;
  h *= 0x846ca68bu;
  h ^= h >> 16;
  return h;
}

float luminance(const vm::vec3f& color)
{
  return 0.2126f * color.x() + 0.7152f * color.y() + 0.0722f * color.z();
}

vm::vec3f multiply(const vm::vec3f& lhs, const vm::vec3f& rhs)
{
  return vm::vec3f{lhs.x() * rhs.x(), lhs.y() * rhs.y(), lhs.z() * rhs.z()};
}

vm::vec3f maximum(const vm::vec3f& lhs, const vm::vec3f& rhs)
{
  return vm::vec3f{
    std::max(lhs.x(), rhs.x()), std::max(lhs.y(), rhs.y()), std::max(lhs.z(), rhs.z())};
}

/**
 * An orthonormal basis around the given unit vector, built without branching on which
 * axis the vector is closest to.
 */
void makeBasis(const vm::vec3f& n, vm::vec3f& tangent, vm::vec3f& bitangent)
{
  const auto sign = std::copysign(1.0f, n.z());
  const auto a = -1.0f / (sign + n.z());
  const auto b = n.x() * n.y() * a;
  tangent = vm::vec3f{1.0f + sign * n.x() * n.x() * a, sign * b, -sign * n.x()};
  bitangent = vm::vec3f{b, sign + n.y() * n.y() * a, -n.y()};
}

/**
 * A direction drawn from the cosine weighted hemisphere around n, which is the
 * distribution a diffuse surface scatters light in.
 */
vm::vec3f sampleCosineHemisphere(const vm::vec3f& n, Rng& rng)
{
  const auto u1 = rng.next();
  const auto u2 = rng.next();

  const auto radius = std::sqrt(u1);
  const auto phi = vm::constants<float>::two_pi() * u2;
  const auto z = std::sqrt(std::max(0.0f, 1.0f - u1));

  auto tangent = vm::vec3f{};
  auto bitangent = vm::vec3f{};
  makeBasis(n, tangent, bitangent);

  return vm::normalize(
    tangent * (radius * std::cos(phi)) + bitangent * (radius * std::sin(phi)) + n * z);
}

/**
 * A direction drawn uniformly from a cone of the given half angle around n. Used to give
 * the sun an angular size, which is what softens its shadows.
 */
vm::vec3f sampleCone(const vm::vec3f& n, const float cosHalfAngle, Rng& rng)
{
  if (cosHalfAngle >= 1.0f)
  {
    return n;
  }

  const auto u1 = rng.next();
  const auto u2 = rng.next();

  const auto cosTheta = 1.0f - u1 * (1.0f - cosHalfAngle);
  const auto sinTheta = std::sqrt(std::max(0.0f, 1.0f - cosTheta * cosTheta));
  const auto phi = vm::constants<float>::two_pi() * u2;

  auto tangent = vm::vec3f{};
  auto bitangent = vm::vec3f{};
  makeBasis(n, tangent, bitangent);

  return vm::normalize(
    tangent * (sinTheta * std::cos(phi)) + bitangent * (sinTheta * std::sin(phi))
    + n * cosTheta);
}

/** A point drawn uniformly from the ball of the given radius. */
vm::vec3f sampleBall(const float radius, Rng& rng)
{
  if (radius <= 0.0f)
  {
    return vm::vec3f{0, 0, 0};
  }

  for (auto attempt = 0; attempt < 8; ++attempt)
  {
    const auto candidate = vm::vec3f{
      rng.next() * 2.0f - 1.0f, rng.next() * 2.0f - 1.0f, rng.next() * 2.0f - 1.0f};
    if (vm::squared_length(candidate) <= 1.0f)
    {
      return candidate * radius;
    }
  }

  return vm::vec3f{0, 0, 0};
}

/**
 * Lifts a ray origin off the surface it starts on, so that it does not immediately hit
 * the triangle it came from. The offset grows with distance from the world origin because
 * that is where floating point precision goes.
 */
float rayEpsilon(const vm::vec3f& position)
{
  return 0.05f * (1.0f + vm::length(position) * (1.0f / 1024.0f));
}

vm::vec3f offsetOrigin(const vm::vec3f& position, const vm::vec3f& normal)
{
  return position + normal * rayEpsilon(position);
}

/**
 * How far a shadow ray from origin towards target may travel before it counts as having
 * arrived.
 *
 * The margin is relative rather than absolute: an absolute one that happened to match the
 * offset the ray origin was already lifted by would leave the far end landing exactly on
 * the surface being aimed at, which is how a light directly overhead ends up shadowing
 * itself.
 */
float shadowRayDistance(const vm::vec3f& origin, const vm::vec3f& target)
{
  return vm::length(target - origin) * 0.999f;
}

/**
 * Evaluates a light's brightness at the given distance, following the attenuation
 * formulas the "delay" key selects.
 *
 * A negative "light" value subtracts light rather than adding it, which mappers use to
 * carve shadow out of an over lit room, so the linear case is careful to fade a negative
 * light towards zero from below rather than clamping it away.
 */
float attenuatedValue(
  const PreviewLight& light, const float distance, const float distanceScale)
{
  if (
    light.attenuation == PreviewAttenuation::None
    || light.attenuation == PreviewAttenuation::LocalMinLight)
  {
    return light.intensity;
  }

  // An explicit falloff overrides the formula: the light fades linearly and reaches zero
  // exactly at the given distance, whatever "wait" says. The compilers only honour it on
  // linear lights, so neither does this.
  if (light.falloff > 0.0f && light.attenuation == PreviewAttenuation::Linear)
  {
    return distance >= light.falloff
             ? 0.0f
             : light.intensity * (1.0f - distance / light.falloff);
  }

  auto value = distanceScale * light.wait * distance;

  switch (light.attenuation)
  {
  case PreviewAttenuation::Inverse:
    return value > 0.0f ? light.intensity / (value / AttenuationScale) : light.intensity;
  case PreviewAttenuation::InverseSquareOffset:
    value += AttenuationScale;
    [[fallthrough]];
  case PreviewAttenuation::InverseSquare:
    return value > 0.0f
             ? light.intensity / ((value * value) / (AttenuationScale * AttenuationScale))
             : light.intensity;
  case PreviewAttenuation::Linear:
    return light.intensity > 0.0f ? std::max(light.intensity - value, 0.0f)
                                  : std::min(light.intensity + value, 0.0f);
  case PreviewAttenuation::None:
  case PreviewAttenuation::LocalMinLight:
    break;
  }

  return light.intensity;
}

/**
 * How much the angle between the surface and the light matters.
 *
 * The compilers do not use the plain cosine here: "_anglescale" mixes it towards a flat
 * response, which softens the terminator on curved brushwork. The scale runs the whole
 * way, from zero, where the angle has no effect on brightness at all, to one, where the
 * plain cosine is used. It defaults to half way between.
 */
float angleTerm(const float cosTheta, const float angleScale)
{
  return (1.0f - angleScale) + angleScale * cosTheta;
}

/**
 * The cosine a light sees the surface at, or nothing when the light is behind it.
 *
 * A light contributes nothing at all to a surface turned away from it, however shallow
 * the angle. "_bleed" is what lets one round the corner onto the back of a thin wall:
 * the size of the angle is taken and its sign thrown away.
 */
std::optional<float> incidence(const float cosTheta, const bool bleed)
{
  const auto value = bleed ? std::abs(cosTheta) : cosTheta;
  return value > 0.0f ? std::optional{value} : std::nullopt;
}

/**
 * The fraction of a spotlight that reaches a point, from the cone and, if the light has
 * one, from the texture it projects.
 */
float spotTerm(
  const PreviewLight& light,
  const PreviewScene& scene,
  const vm::vec3f& toSurface,
  const float distance)
{
  if (!light.spot)
  {
    return 1.0f;
  }

  auto result = 1.0f;

  const auto cosTheta = vm::dot(light.direction, toSurface);
  if (cosTheta <= light.cosOuterCone)
  {
    return 0.0f;
  }

  if (light.cosInnerCone > light.cosOuterCone && cosTheta < light.cosInnerCone)
  {
    result = (cosTheta - light.cosOuterCone) / (light.cosInnerCone - light.cosOuterCone);
  }

  if (light.projectedTextureIndex >= 0)
  {
    // Unwrap the surface point into the light's projection frame. The frustum is square,
    // so a point outside it in either axis receives nothing.
    const auto delta = toSurface * distance;
    const auto forward = vm::dot(delta, light.direction);
    if (forward <= 0.0f)
    {
      return 0.0f;
    }

    const auto extent = forward * light.projectTanHalfFov;
    const auto x = vm::dot(delta, light.projectRight);
    const auto y = vm::dot(delta, light.projectUp);
    if (std::abs(x) > extent || std::abs(y) > extent)
    {
      return 0.0f;
    }

    const auto uv = vm::vec2f{0.5f + 0.5f * x / extent, 0.5f - 0.5f * y / extent};
    const auto& material = *scene.materials[size_t(light.projectedTextureIndex)];
    result *= luminance(material.sample(uv));
  }

  return result;
}

/**
 * The radiance the sky shows in the given direction, in display units.
 *
 * "_sunlight2" lights the upper hemisphere and "_sunlight3" the lower one, and both are
 * emitted by sky faces rather than by the whole sphere, which is why a map without sky
 * brushes gets nothing from either. The suns themselves are not included: they are
 * sampled directly, and adding them here as well would count them twice.
 */
vm::vec3f skyRadiance(const PreviewScene& scene, const vm::vec3f& direction)
{
  const auto& dome =
    direction.z() >= 0.0f ? scene.globals.skyDome : scene.globals.groundDome;
  return dome * PreviewLightUnitScale;
}

/**
 * Whether the texture has a hole where the ray struck, in which case there is nothing
 * there to stop it, shade it or bounce it.
 *
 * Called for every triangle a ray actually hits, so it does as little as it can: a
 * triangle whose texture has no holes at all is turned away by a flag, without the
 * material being reached for.
 */
bool hitAHole(
  const PreviewScene& scene, const uint32_t triangleIndex, const float u, const float v)
{
  const auto& shading = scene.triangleShading[triangleIndex];
  if (!shading.maskedTexture)
  {
    return false;
  }

  const auto w = 1.0f - u - v;
  const auto uv = shading.uv0 * w + shading.uv1 * u + shading.uv2 * v;
  return scene.materials[shading.materialIndex]->transparentAt(uv);
}

/**
 * The permutation the mottle noise is built on, taken from ericw-tools so that a preview
 * breaks its minimum light up in the same places a compile does. Its light tool is under
 * the GPL, version 2 or later, which this is a use of under version 3.
 */
constexpr uint8_t MottlePermutation[256] = {
  11,  255, 250, 82,  217, 9,   144, 93,  136, 153, 55,  71,  73,  204, 96,  180,
  126, 8,   50,  46,  113, 91,  238, 143, 30,  215, 191, 243, 65,  58,  208, 33,
  86,  1,   182, 118, 83,  115, 207, 52,  94,  112, 205, 48,  99,  254, 117, 101,
  157, 140, 72,  242, 244, 154, 10,  135, 155, 168, 125, 183, 148, 116, 187, 166,
  25,  156, 177, 231, 165, 57,  221, 105, 28,  211, 127, 41,  142, 253, 146, 87,
  122, 229, 162, 137, 194, 174, 167, 15,  220, 26,  235, 3,   39,  80,  88,  42,
  202, 12,  97,  53,  70,  123, 170, 110, 214, 192, 173, 84,  169, 188, 64,  102,
  147, 158, 100, 69,  213, 193, 43,  20,  13,  237, 171, 103, 32,  190, 223, 150,
  131, 206, 85,  124, 163, 18,  139, 132, 79,  29,  216, 232, 178, 74,  24,  141,
  201, 181, 152, 4,   7,   159, 134, 212, 226, 245, 164, 239, 47,  66,  27,  40,
  197, 81,  78,  219, 228, 241, 121, 23,  120, 230, 76,  252, 199, 184, 45,  203,
  161, 89,  16,  21,  119, 5,   209, 196, 68,  130, 195, 176, 225, 233, 128, 22,
  248, 179, 249, 61,  108, 138, 145, 31,  49,  107, 56,  172, 224, 210, 6,   160,
  189, 104, 200, 44,  175, 133, 77,  62,  106, 92,  186, 227, 14,  38,  247, 37,
  17,  222, 36,  75,  129, 185, 251, 240, 54,  151, 2,   98,  149, 0,   63,  218,
  60,  198, 19,  59,  90,  246, 234, 67,  51,  109, 95,  236, 35,  34,  114, 111};

/**
 * A slow, smooth noise from nought to forty eight, which "_minlight_mottle" adds to the
 * minimum light so that a surface lit by nothing else does not read as a flat wash.
 *
 * A value is looked up at each of the eight lattice points around the position, sixteen
 * units apart, and mixed between them.
 */
float mottle(const vm::vec3f& position)
{
  const auto scaled = position * (1.0f / 16.0f);

  const auto floorOf = [](const float v) { return int64_t(std::floor(v)); };
  const auto x = floorOf(scaled.x());
  const auto y = floorOf(scaled.y());
  const auto z = floorOf(scaled.z());

  const auto fx = float(scaled.x() - float(x));
  const auto fy = float(scaled.y() - float(y));
  const auto fz = float(scaled.z() - float(z));

  const auto at = [](const int64_t px, const int64_t py, const int64_t pz) {
    const auto wrap = [](const int64_t v) { return size_t(((v % 256) + 256) % 256); };
    auto value = MottlePermutation[wrap(px)];
    value = MottlePermutation[wrap(int64_t(value) + py)];
    value = MottlePermutation[wrap(int64_t(value) + pz)];
    return float(value);
  };

  const auto mix = [](const float a, const float b, const float t) {
    return a + (b - a) * t;
  };

  const auto z0 = mix(
    mix(at(x, y, z), at(x + 1, y, z), fx),
    mix(at(x, y + 1, z), at(x + 1, y + 1, z), fx),
    fy);
  const auto z1 = mix(
    mix(at(x, y, z + 1), at(x + 1, y, z + 1), fx),
    mix(at(x, y + 1, z + 1), at(x + 1, y + 1, z + 1), fx),
    fy);

  return mix(z0, z1, fz) / 255.0f * 48.0f;
}

/**
 * How many directions a surface looks in for what is closing in on it.
 *
 * The compilers take forty eight every time, which they can afford once per lightmap
 * sample. A preview is drawn over and over and averages what it finds, so it takes four
 * at random each pass and lets the passes fill in the rest; with the gain left at one,
 * which is where it shapes the result linearly, that comes to the same answer.
 *
 * Four is what the cost will bear: a scene asking for dirt takes about twice as long to
 * trace as one that does not, where forty eight would be ten times.
 */
constexpr auto DirtRaysPerSample = 4;

/**
 * How closed in a surface is, from nought where nothing is near it to one where it is
 * shut in on every side.
 *
 * Rays are cast within "_dirtangle" of the normal and each is followed for at most
 * "_dirtdepth"; what comes back is how much of that depth they got through on average,
 * turned around so that more occlusion is a larger number.
 */
float surfaceOcclusion(
  const PreviewScene& scene,
  const vm::vec3f& position,
  const vm::vec3f& normal,
  const int32_t objectChannelMask,
  Rng& rng)
{
  const auto& globals = scene.globals;
  const auto depth = globals.dirtDepth;
  const auto cosMaxAngle = std::cos(vm::to_radians(globals.dirtAngle));

  const auto origin = offsetOrigin(position, normal);
  auto totalDistance = 0.0f;

  for (auto i = 0; i < DirtRaysPerSample; ++i)
  {
    const auto direction = sampleCone(normal, cosMaxAngle, rng);
    const auto ray = PreviewRay{origin, direction};

    // A surface only looks for what is on its own channel, so a model moved off the
    // default one is not shut in by geometry it does not share a channel with.
    const auto hit = scene.bvh.intersect(
      scene.trianglePositions,
      ray,
      depth,
      [&](const uint32_t triangleIndex) {
        const auto& shading = scene.triangleShading[triangleIndex];
        return shading.occludes && (shading.objectChannelMask & objectChannelMask) != 0;
      },
      [&](const uint32_t triangleIndex, const float u, const float v) {
        return !hitAHole(scene, triangleIndex, u, v);
      });

    totalDistance += hit ? std::min(depth, hit->distance) : depth;
  }

  const auto averageDistance = totalDistance / float(DirtRaysPerSample);
  return std::clamp(1.0f - averageDistance / depth, 0.0f, 1.0f);
}

/**
 * How much of a light survives the dirt at a point, from one where none of it is taken
 * away to zero where all of it is.
 *
 * A light can be told to take part or stay out of it, and can carry a scale and a gain
 * of its own; one that carries both radii fades its dirt in between them, measured from
 * the light, so that nothing close to it is darkened.
 */
float dirtScaleFactor(
  const PreviewGlobalLighting& globals,
  const PreviewLight* light,
  const float occlusion,
  const float distance)
{
  if (!globals.dirtInUse)
  {
    return 1.0f;
  }

  // No light means the caller wants dirt whatever the map says, which is how the sky and
  // the minimum light ask for it.
  const auto useDirt = !light            ? true
                       : light->dirt < 0 ? false
                       : light->dirt > 0 ? true
                                         : globals.dirt;
  if (!useDirt || occlusion <= 0.0f)
  {
    return 1.0f;
  }

  const auto gain = light && light->dirtGain > 0.0f ? light->dirtGain : globals.dirtGain;
  const auto scale =
    light && light->dirtScale > 0.0f ? light->dirtScale : globals.dirtScale;

  auto dirt = std::min(std::pow(occlusion, gain), 1.0f);
  dirt = std::min(dirt * scale, 1.0f);

  if (light && light->dirtRadiusSet)
  {
    if (distance < light->dirtOffRadius)
    {
      dirt = 0.0f;
    }
    else if (distance < light->dirtOnRadius)
    {
      const auto span = light->dirtOnRadius - light->dirtOffRadius;
      dirt *= span > 0.0f ? (distance - light->dirtOffRadius) / span : 1.0f;
    }
  }

  return 1.0f - dirt;
}

/**
 * Whether anything opaque stands between two points.
 *
 * Only surfaces that are part of the solid hull block the ray. Water, triggers and clip
 * brushes do not, and neither does sky, which is what lets a shadow ray reach a sun.
 */
bool occluded(
  const PreviewScene& scene,
  const vm::vec3f& origin,
  const vm::vec3f& direction,
  const float distance,
  const int32_t shadowChannelMask,
  const int32_t receiverObjectIndex)
{
  const auto ray = PreviewRay{origin, direction};
  return scene.bvh.occluded(
    scene.trianglePositions,
    ray,
    distance,
    [&](const uint32_t triangleIndex) {
      const auto& shading = scene.triangleShading[triangleIndex];
      if (!shading.occludes || (shading.objectChannelMask & shadowChannelMask) == 0)
      {
        return false;
      }

      // A model told to shadow itself, or the world, alone is in the way of nothing
      // else: the light carries on past it onto everything it was not asked to darken.
      if (shading.shadowsSelfOnly && shading.objectIndex != receiverObjectIndex)
      {
        return false;
      }
      if (shading.shadowsWorldOnly && receiverObjectIndex != 0)
      {
        return false;
      }

      return true;
    },
    [&](const uint32_t triangleIndex, const float u, const float v) {
      return !hitAHole(scene, triangleIndex, u, v);
    });
}

/**
 * One light that might reach the shading point, with what it would contribute if nothing
 * stood in the way.
 */
struct LightCandidate
{
  // Left deliberately without default values: a shading point declares a whole array of
  // these and fills in only the lights that reach it, and zeroing the rest first is
  // several kilobytes of writes per shading point for nothing.
  const PreviewLight* light;
  vm::vec3f contribution;
  vm::vec3f toLight;
  /** Where the light was sampled. Meaningless for a sun, which has no position. */
  vm::vec3f samplePosition;
  bool infinite;
  float weight;
};

/**
 * Works out what one light would contribute to the shading point before shadowing.
 *
 * Returns false for the lights that cannot reach it at all, which is most of them in a
 * map of any size, and is why this runs before any ray is traced.
 */
bool evaluateLight(
  const PreviewScene& scene,
  const PreviewLight& light,
  const vm::vec3f& position,
  const vm::vec3f& normal,
  Rng& rng,
  LightCandidate& candidate)
{
  candidate.light = &light;
  candidate.infinite = light.kind == PreviewLightKind::Sun;
  candidate.samplePosition = light.origin;

  if (light.kind == PreviewLightKind::Sun)
  {
    // A sun has no position: it shines from infinitely far away, so every point that
    // faces it sees the same brightness.
    const auto toLight = sampleCone(
      -light.direction,
      light.penumbra > 0.0f ? std::cos(vm::to_radians(light.penumbra)) : 1.0f,
      rng);

    const auto cosTheta = incidence(vm::dot(normal, toLight), light.bleed);
    if (!cosTheta)
    {
      return false;
    }

    candidate.toLight = toLight;
    candidate.infinite = true;
    candidate.contribution =
      light.color
      * (light.intensity * light.styleScale * angleTerm(*cosTheta, light.angleScale));
  }
  else
  {
    const auto origin = light.origin + sampleBall(light.deviance, rng);
    const auto delta = origin - position;
    const auto distanceSquared = vm::squared_length(delta);
    if (distanceSquared < 1.0e-6f)
    {
      return false;
    }

    const auto distance = std::sqrt(distanceSquared);
    const auto toLight = delta / distance;

    const auto cosTheta = incidence(vm::dot(normal, toLight), light.bleed);
    if (!cosTheta)
    {
      return false;
    }

    const auto spot = spotTerm(light, scene, -toLight, distance);
    if (spot <= 0.0f)
    {
      return false;
    }

    const auto value =
      attenuatedValue(light, distance, scene.globals.distScale) * light.styleScale;
    if (value == 0.0f)
    {
      return false;
    }

    candidate.toLight = toLight;
    candidate.samplePosition = origin;

    if (light.kind == PreviewLightKind::LocalMinLight)
    {
      // A local minimum light does not fall off and does not add to what is already
      // there; it only lifts whatever it can see up to its own value.
      candidate.contribution = light.color * value * spot;
    }
    else
    {
      candidate.contribution =
        light.color * (value * spot * angleTerm(*cosTheta, light.angleScale));
    }
  }

  candidate.weight = std::abs(luminance(candidate.contribution));
  return candidate.weight > 1.0e-5f;
}

/**
 * Gathers the light entities reaching the shading point and traces shadow rays to them.
 *
 * A map can hold hundreds of lights, so a point that many of them reach picks a handful
 * in proportion to how much they would contribute and scales the result back up. That
 * bounds the shadow rays per sample without biasing the answer: the passes still average
 * to the same image, they just get there with a little more noise in the rare rooms where
 * it matters.
 */
vm::vec3f gatherDirectLight(
  const PreviewScene& scene,
  const PreviewTraceSettings& settings,
  const vm::vec3f& position,
  const vm::vec3f& normal,
  const PreviewTriangleShading& shading,
  const bool bounceSource,
  const float occlusion,
  vm::vec3f& localMinLight,
  Rng& rng)
{
  auto result = vm::vec3f{0, 0, 0};

  LightCandidate candidates[MaxLightCandidates];
  auto candidateCount = size_t(0);
  auto totalWeight = 0.0f;

  for (const auto& light : scene.lights)
  {
    if ((light.lightChannelMask & shading.objectChannelMask) == 0)
    {
      continue;
    }

    if (candidateCount >= MaxLightCandidates)
    {
      break;
    }

    if (evaluateLight(scene, light, position, normal, rng, candidates[candidateCount]))
    {
      totalWeight += candidates[candidateCount].weight;
      ++candidateCount;
    }
  }

  if (candidateCount == 0)
  {
    return result;
  }

  const auto shadowOrigin = offsetOrigin(position, normal);

  const auto accumulate = [&](const LightCandidate& candidate, const float scale) {
    const auto& light = *candidate.light;
    const auto distance = candidate.infinite
                            ? MaxRayDistance
                            : shadowRayDistance(shadowOrigin, candidate.samplePosition);
    if (distance <= 0.0f)
    {
      return;
    }

    if (occluded(
          scene,
          shadowOrigin,
          candidate.toLight,
          distance,
          light.shadowChannelMask,
          shading.objectIndex))
    {
      return;
    }

    const auto dirt = dirtScaleFactor(
      scene.globals, &light, occlusion, candidate.infinite ? 0.0f : distance);

    if (light.kind == PreviewLightKind::LocalMinLight)
    {
      // Non additive: the brightest local minimum light that can see this point wins.
      localMinLight = maximum(localMinLight, candidate.contribution * dirt);
    }
    else
    {
      // A light's own "_bouncescale" says how much of it is allowed to bounce, so it
      // applies to the light landing on a surface the path is about to bounce off, not
      // to the light the camera sees directly. A light carrying a style does not bounce
      // at all unless the map asks for it, since a bounce cannot be switched along with
      // what cast it.
      const auto styledAndUnbounced = light.style != 0 && !scene.globals.bounceStyled;
      const auto bounceScale =
        bounceSource ? (styledAndUnbounced ? 0.0f : light.bounceScale) : 1.0f;
      result = result + candidate.contribution * (scale * bounceScale * dirt);
    }
  };

  const auto shadowRays =
    std::min(size_t(std::max(settings.maxShadowRays, 1)), candidateCount);

  if (shadowRays == candidateCount)
  {
    for (size_t i = 0; i < candidateCount; ++i)
    {
      accumulate(candidates[i], 1.0f);
    }
    return result;
  }

  if (totalWeight <= 0.0f)
  {
    return result;
  }

  for (size_t sample = 0; sample < shadowRays; ++sample)
  {
    auto target = rng.next() * totalWeight;
    auto chosen = candidateCount - 1;
    for (size_t i = 0; i < candidateCount; ++i)
    {
      target -= candidates[i].weight;
      if (target <= 0.0f)
      {
        chosen = i;
        break;
      }
    }

    const auto probability = candidates[chosen].weight / totalWeight * float(shadowRays);
    if (probability > 0.0f)
    {
      accumulate(candidates[chosen], 1.0f / probability);
    }
  }

  return result;
}

/**
 * Samples the emissive triangles, which is how surface lights and Quake 2's light
 * flagged faces reach a surface.
 *
 * One triangle is picked in proportion to its area, so a large emitter is sampled as
 * often as it deserves, and the estimate is scaled by the total emitting area to make up
 * for only having looked at one of them.
 */
vm::vec3f gatherEmitters(
  const PreviewScene& scene,
  const vm::vec3f& position,
  const vm::vec3f& normal,
  const int32_t receiverObjectIndex,
  Rng& rng)
{
  if (scene.emitters.empty() || scene.totalEmitterArea <= 0.0f)
  {
    return vm::vec3f{0, 0, 0};
  }

  const auto target = rng.next() * scene.totalEmitterArea;
  const auto it = std::lower_bound(
    scene.emitters.begin(),
    scene.emitters.end(),
    target,
    [](const PreviewEmitter& emitter, const float value) {
      return emitter.cumulativeArea < value;
    });
  const auto& emitter = it != scene.emitters.end() ? *it : scene.emitters.back();

  const auto& emitterPosition = scene.trianglePositions[emitter.triangleIndex];
  const auto& emitterShading = scene.triangleShading[emitter.triangleIndex];

  // Uniform barycentric coordinates, folded back into the triangle when they land in the
  // mirrored half of the unit square.
  auto u = rng.next();
  auto v = rng.next();
  if (u + v > 1.0f)
  {
    u = 1.0f - u;
    v = 1.0f - v;
  }

  // A hole in the texture emits nothing. The emitter's share of the sampling still
  // counts its whole triangle, so turning these samples away is what scales its
  // contribution down to the part of it that is actually there.
  if (hitAHole(scene, emitter.triangleIndex, u, v))
  {
    return vm::vec3f{0, 0, 0};
  }

  const auto samplePoint =
    emitterPosition.p0 + emitterPosition.e1 * u + emitterPosition.e2 * v;

  const auto delta = samplePoint - position;
  const auto distanceSquared = vm::squared_length(delta);
  if (distanceSquared < 1.0e-4f)
  {
    return vm::vec3f{0, 0, 0};
  }

  const auto distance = std::sqrt(distanceSquared);
  const auto toLight = delta / distance;

  const auto cosSurface = vm::dot(normal, toLight);
  if (cosSurface <= 0.0f)
  {
    return vm::vec3f{0, 0, 0};
  }

  // An emitter radiates from its front face only, so take the cosine at the light end
  // with the sign of the side we are looking at.
  // A sky gives off light in every direction rather than out of the face it is drawn on,
  // so which way that face points does not come into it; a surface light does.
  const auto cosLight = emitterShading.omnidirectionalEmitter
                          ? 0.5f
                          : -vm::dot(emitterShading.normal, toLight);
  if (cosLight <= 0.0f)
  {
    return vm::vec3f{0, 0, 0};
  }

  // Surface lights always live on channel 1, so every brush model that has not been moved
  // off that channel casts a shadow from them.
  const auto shadowOrigin = offsetOrigin(position, normal);
  if (occluded(
        scene,
        shadowOrigin,
        toLight,
        shadowRayDistance(shadowOrigin, samplePoint),
        1,
        receiverObjectIndex))
  {
    return vm::vec3f{0, 0, 0};
  }

  // "_surflightskydist" pushes a sky further away than it is, and "_surflight_atten"
  // stretches the distance a surface is measured over, so one asked to fade twice as
  // fast reads as if it were twice as far away.
  const auto atten = emitterShading.emissionAtten;
  const auto measuredDistance =
    (distance + emitterShading.emissionDistanceOffset) * (atten > 0.0f ? atten : 1.0f);
  const auto attenuatedDistanceSquared = measuredDistance * measuredDistance;

  const auto geometry = cosSurface * cosLight / attenuatedDistanceSquared;
  return emitterShading.emission
         * (geometry * scene.totalEmitterArea / vm::constants<float>::pi());
}

} // namespace

vm::vec3f PreviewCamera::rayDirection(const float x, const float y) const
{
  // The viewport's y axis points down while the camera's up vector points up, hence the
  // flip; both are measured from the centre of the view.
  const auto ndcX = 2.0f * x / float(width) - 1.0f;
  const auto ndcY = 1.0f - 2.0f * y / float(height);

  return vm::normalize(forward + right * (ndcX * halfWidth) + up * (ndcY * halfHeight));
}

bool PreviewCamera::operator==(const PreviewCamera& other) const
{
  return position == other.position && forward == other.forward && right == other.right
         && up == other.up && halfWidth == other.halfWidth
         && halfHeight == other.halfHeight && width == other.width
         && height == other.height;
}

PreviewCamera makePreviewCamera(
  const gl::Camera& camera, const int width, const int height)
{
  auto result = PreviewCamera{};
  result.position = camera.position();
  result.forward = camera.direction();
  result.right = camera.right();
  result.up = camera.up();
  result.width = std::max(width, 1);
  result.height = std::max(height, 1);

  // Recover the view plane from two corners of the camera's own frustum, so that the
  // preview lines up with the rendered scene whatever projection and zoom are in use.
  const auto topLeft = camera.unproject(0.0f, 0.0f, 0.5f) - camera.position();
  const auto forwardExtent = vm::dot(topLeft, result.forward);
  if (forwardExtent > vm::constants<float>::almost_zero())
  {
    const auto scale = 1.0f / forwardExtent;
    result.halfWidth = std::abs(vm::dot(topLeft, result.right)) * scale;
    result.halfHeight = std::abs(vm::dot(topLeft, result.up)) * scale;
  }

  return result;
}

int32_t effectiveBounces(const PreviewScene& scene, const PreviewTraceSettings& settings)
{
  // Following the map means using the map's own bounce count, which is zero unless the
  // mapper set "_bounce". Overriding it uses the preview's count instead, so a mapper can
  // see what bouncing would do before committing the key.
  return std::clamp(
    settings.indirectLight == PreviewIndirectLight::On    ? settings.maxBounces
    : settings.indirectLight == PreviewIndirectLight::Off ? 0
                                                          : scene.globals.bounces,
    0,
    MaxPreviewBounces);
}

vm::vec3f tracePreviewPixel(
  const PreviewScene& scene,
  const PreviewCamera& camera,
  const PreviewTraceSettings& settings,
  const int32_t x,
  const int32_t y,
  const uint32_t sampleIndex)
{
  auto rng =
    Rng{hashSeed(uint32_t(y) * uint32_t(camera.width) + uint32_t(x), sampleIndex)};

  // Jitter within the pixel, which antialiases the image for free as the passes average.
  const auto pixelX = float(x) + rng.next();
  const auto pixelY = float(y) + rng.next();

  auto origin = camera.position;
  auto direction = camera.rayDirection(pixelX, pixelY);

  auto radiance = vm::vec3f{0, 0, 0};
  auto throughput = vm::vec3f{1, 1, 1};

  const auto maxBounces = effectiveBounces(scene, settings);

  auto depth = 0;
  auto passThroughs = 0;

  while (true)
  {
    const auto ray = PreviewRay{origin, direction};
    const auto hit = scene.bvh.intersect(
      scene.trianglePositions,
      ray,
      MaxRayDistance,
      [](const uint32_t) { return true; },
      [&](const uint32_t triangleIndex, const float u, const float v) {
        // A hole is not there to be seen or bounced off either, so the ray simply
        // carries on to whatever is behind it.
        return !hitAHole(scene, triangleIndex, u, v);
      });

    if (!hit)
    {
      // Nothing was hit, so the ray left the map. Outside a sealed map that is not
      // meaningful, but inside one it only happens through a hole, where showing the sky
      // is the friendlier answer.
      radiance = radiance + multiply(throughput, skyRadiance(scene, direction));
      break;
    }

    const auto& shading = scene.triangleShading[hit->triangleIndex];

    if (shading.kind == PreviewSurfaceKind::Sky)
    {
      if (depth == 0)
      {
        // What the camera sees of a sky face is the sky texture, not the light it lets
        // in, so show the texture's own colour rather than the dome's.
        radiance =
          radiance
          + multiply(throughput, scene.materials[shading.materialIndex]->averageColor);
      }
      else
      {
        radiance = radiance + multiply(throughput, skyRadiance(scene, direction));
      }
      break;
    }

    const auto position = origin + direction * hit->distance;

    const auto w = 1.0f - hit->u - hit->v;

    // A surface the map asks to be smoothed is shaded with the normal its corners carry
    // rather than the one its plane has, which is what rounds off brushwork built as a
    // run of flat faces.
    const auto geometricNormal =
      shading.smoothIndex >= 0
        ? vm::normalize(
            scene.smoothNormals[size_t(shading.smoothIndex)].normal0 * w
            + scene.smoothNormals[size_t(shading.smoothIndex)].normal1 * hit->u
            + scene.smoothNormals[size_t(shading.smoothIndex)].normal2 * hit->v)
        : shading.normal;

    // Faces are one sided, but the camera can end up behind one, so shade whichever side
    // the ray arrived on. Which side that is comes from the plane rather than from a
    // smoothed normal, which can lean past the edge of the surface it belongs to.
    const auto normal =
      vm::dot(shading.normal, direction) < 0.0f ? geometricNormal : -geometricNormal;

    const auto uv = shading.uv0 * w + shading.uv1 * hit->u + shading.uv2 * hit->v;
    const auto albedo = scene.materials[shading.materialIndex]->sample(uv);

    // What an emitting surface shows of itself, which "_surflight_minlight_scale" can
    // turn down without taking anything away from what it gives off.
    auto surface =
      depth == 0 ? shading.emission * shading.selfEmissionScale : vm::vec3f{0, 0, 0};

    auto irradiance = vm::vec3f{0, 0, 0};
    auto localMinLight = vm::vec3f{0, 0, 0};

    // How shut in the point is, which the lights reaching it are then darkened by
    // according to what each of them and the map ask for. Nothing looks unless something
    // in the map has asked for dirt, since looking costs a sheaf of rays.
    const auto occlusion =
      scene.globals.dirtInUse && !shading.noDirt && shading.receivesLight
        ? surfaceOcclusion(scene, position, normal, shading.objectChannelMask, rng)
        : 0.0f;

    if (shading.receivesLight)
    {
      irradiance = gatherDirectLight(
        scene,
        settings,
        position,
        normal,
        shading,
        depth > 0,
        occlusion,
        localMinLight,
        rng);
    }

    if (shading.receivesLight)
    {
      // A surface light is a light like any other, so it belongs in the lightmap the
      // steps below act on rather than beside it. Its contribution comes back in display
      // units, which is what dividing puts back.
      irradiance = irradiance
                   + gatherEmitters(scene, position, normal, shading.objectIndex, rng)
                       / PreviewLightUnitScale;
    }

    // The three kinds of minimum light: the one a brush model carries, which stands in
    // for the map's, the map's own, and whatever a delay 4 light in line of sight
    // provides. The compilers put them in while lighting, so they are scaled by
    // everything below like any other light.
    //
    // They are floors under what a surface receives rather than contributions to it,
    // unless "_addmin" asks for the opposite.
    const auto modelHasMinLight = vm::squared_length(shading.surfaceMinLight) > 0.0f;
    auto floorLight = modelHasMinLight ? shading.surfaceMinLight : scene.globals.minLight;

    // Broken up by a slow noise, if the surface asks for it, before anything scales it.
    const auto mottled =
      modelHasMinLight ? shading.surfaceMinLightMottle : scene.globals.minLightMottle;
    if (mottled && vm::squared_length(floorLight) > 0.0f)
    {
      const auto color =
        modelHasMinLight ? shading.surfaceMinLightColor : scene.globals.minLightColor;
      floorLight = floorLight + color * mottle(position);
    }

    if (scene.globals.minLightDirt)
    {
      floorLight = floorLight * dirtScaleFactor(scene.globals, nullptr, occlusion, 0.0f);
    }

    floorLight = maximum(floorLight, localMinLight);
    irradiance = scene.globals.addMinLight ? irradiance + floorLight
                                           : maximum(irradiance, floorLight);

    // Subtractive lights can push the total below zero, but a surface cannot emit
    // negative light.
    irradiance = maximum(irradiance, vm::vec3f{0, 0, 0});

    // "_maxlight" is a ceiling on the brightest channel at twice its own value, and the
    // whole colour is brought down to it together rather than each channel being cut off
    // on its own, so that a surface up against it keeps its hue. A brush model carrying
    // one of its own is held to that instead of the map's.
    const auto maxLight =
      shading.surfaceMaxLight > 0.0f ? shading.surfaceMaxLight : scene.globals.maxLight;
    if (maxLight > 0.0f)
    {
      const auto ceiling = maxLight * 2.0f;
      const auto peak =
        std::max(irradiance.x(), std::max(irradiance.y(), irradiance.z()));
      if (peak > ceiling)
      {
        irradiance = irradiance * (ceiling / peak);
      }
    }

    // "_lightcolorscale" takes the colour out of the light a model receives, mixing it
    // towards the grey its brightest channel would be.
    if (shading.lightColorScale < 1.0f)
    {
      const auto grey =
        std::max(irradiance.x(), std::max(irradiance.y(), irradiance.z()));
      irradiance = vm::vec3f{grey, grey, grey}
                   + (irradiance - vm::vec3f{grey, grey, grey}) * shading.lightColorScale;
    }

    // "_range" scales every light's brightness without changing how far it reaches, and
    // comes last, once everything that feeds the lightmap is in.
    irradiance = irradiance * scene.globals.rangeScale;

    const auto outgoing = irradiance * PreviewLightUnitScale;

    surface = surface + multiply(albedo, outgoing);
    radiance = radiance + multiply(throughput, surface * shading.alpha);

    // A liquid is drawn partly see-through, so the ray carries on behind it with whatever
    // of it is left over. Crossing one is not a bounce: a couple of pools of water in a
    // row must not use up everything the path was going to spend on indirect light.
    if (shading.alpha < 1.0f)
    {
      throughput = throughput * (1.0f - shading.alpha);
      ++passThroughs;
      if (passThroughs >= MaxPassThroughs || luminance(throughput) < MinThroughput)
      {
        break;
      }

      origin = position + direction * rayEpsilon(position);
      continue;
    }

    if (depth == maxBounces)
    {
      break;
    }

    // Carry on into the scene. "_bouncecolorscale" controls how much colour the bounce
    // picks up from the surface it came off, which mappers turn down when a saturated
    // texture tints a whole room.
    const auto bounceAlbedo = vm::mix(
      vm::vec3f{luminance(albedo), luminance(albedo), luminance(albedo)},
      albedo,
      vm::vec3f{
        scene.globals.bounceColorScale,
        scene.globals.bounceColorScale,
        scene.globals.bounceColorScale});

    throughput = multiply(throughput, bounceAlbedo * scene.globals.bounceScale);

    if (luminance(throughput) < MinThroughput)
    {
      break;
    }

    direction = sampleCosineHemisphere(normal, rng);
    origin = offsetOrigin(position, normal);
    ++depth;
  }

  return radiance * settings.exposure;
}

} // namespace tb::render
