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
  const int32_t shadowChannelMask)
{
  const auto ray = PreviewRay{origin, direction};
  return scene.bvh.occluded(
    scene.trianglePositions, ray, distance, [&](const uint32_t triangleIndex) {
      const auto& shading = scene.triangleShading[triangleIndex];
      return shading.occludes && (shading.objectChannelMask & shadowChannelMask) != 0;
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

    const auto cosTheta = vm::dot(normal, toLight);
    if (cosTheta <= 0.0f)
    {
      return false;
    }

    candidate.toLight = toLight;
    candidate.infinite = true;
    candidate.contribution =
      light.color
      * (light.intensity * light.styleScale * angleTerm(cosTheta, light.angleScale));
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

    const auto cosTheta = vm::dot(normal, toLight);
    if (cosTheta <= 0.0f)
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
        light.color * (value * spot * angleTerm(cosTheta, light.angleScale));
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
          scene, shadowOrigin, candidate.toLight, distance, light.shadowChannelMask))
    {
      return;
    }

    if (light.kind == PreviewLightKind::LocalMinLight)
    {
      // Non additive: the brightest local minimum light that can see this point wins.
      localMinLight = maximum(localMinLight, candidate.contribution);
    }
    else
    {
      result = result + candidate.contribution * scale;
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
  const PreviewScene& scene, const vm::vec3f& position, const vm::vec3f& normal, Rng& rng)
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
  const auto cosLight = -vm::dot(emitterShading.normal, toLight);
  if (cosLight <= 0.0f)
  {
    return vm::vec3f{0, 0, 0};
  }

  // Surface lights always live on channel 1, so every brush model that has not been moved
  // off that channel casts a shadow from them.
  const auto shadowOrigin = offsetOrigin(position, normal);
  if (occluded(
        scene, shadowOrigin, toLight, shadowRayDistance(shadowOrigin, samplePoint), 1))
  {
    return vm::vec3f{0, 0, 0};
  }

  const auto geometry = cosSurface * cosLight / distanceSquared;
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

  // Following the map means using the map's own bounce count, which is zero unless the
  // mapper set "_bounce". Overriding it uses the preview's count instead, so a mapper can
  // see what bouncing would do before committing the key.
  const auto maxBounces = std::clamp(
    settings.indirectLight == PreviewIndirectLight::On    ? settings.maxBounces
    : settings.indirectLight == PreviewIndirectLight::Off ? 0
                                                          : scene.globals.bounces,
    0,
    MaxPreviewBounces);

  auto depth = 0;
  auto passThroughs = 0;

  while (true)
  {
    const auto ray = PreviewRay{origin, direction};
    const auto hit = scene.bvh.intersect(
      scene.trianglePositions, ray, MaxRayDistance, [](const uint32_t) { return true; });

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

    // Faces are one sided, but the camera can end up behind one, so shade whichever side
    // the ray arrived on.
    const auto geometricNormal = shading.normal;
    const auto normal =
      vm::dot(geometricNormal, direction) < 0.0f ? geometricNormal : -geometricNormal;

    const auto w = 1.0f - hit->u - hit->v;
    const auto uv = shading.uv0 * w + shading.uv1 * hit->u + shading.uv2 * hit->v;
    const auto albedo = scene.materials[shading.materialIndex]->sample(uv);

    auto surface = depth == 0 ? shading.emission : vm::vec3f{0, 0, 0};

    auto irradiance = vm::vec3f{0, 0, 0};
    auto localMinLight = vm::vec3f{0, 0, 0};

    if (shading.receivesLight)
    {
      irradiance =
        gatherDirectLight(scene, settings, position, normal, shading, localMinLight, rng);
    }

    // "_range" scales every light's brightness without changing how far it reaches.
    irradiance = irradiance * scene.globals.rangeScale;

    // Subtractive lights can push the total below zero, but a surface cannot emit
    // negative light.
    irradiance = maximum(irradiance, vm::vec3f{0, 0, 0});

    if (scene.globals.maxLight > 0.0f)
    {
      irradiance = vm::vec3f{
        std::min(irradiance.x(), scene.globals.maxLight),
        std::min(irradiance.y(), scene.globals.maxLight),
        std::min(irradiance.z(), scene.globals.maxLight)};
    }

    // The three kinds of minimum light are all floors rather than contributions: the
    // global one, the one a brush model carries, and whatever a delay 4 light in line of
    // sight provides.
    auto floorLight = maximum(scene.globals.minLight, shading.surfaceMinLight);
    floorLight = maximum(floorLight, localMinLight);
    irradiance = maximum(irradiance, floorLight);

    auto outgoing = irradiance * PreviewLightUnitScale;

    if (shading.receivesLight)
    {
      outgoing = outgoing + gatherEmitters(scene, position, normal, rng);
    }

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
