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

#pragma once

#include "render/LightPreviewBvh.h"
#include "render/LightPreviewLights.h"

#include "vm/bbox.h"
#include "vm/vec.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace tb::gl
{
class Gl;
class Material;
} // namespace tb::gl

namespace tb::mdl
{
class Map;
}

namespace tb::render
{

/**
 * What one unit of the "light" key is worth on screen.
 *
 * The compilers do not write a fully lit surface as 255. The engines light the world
 * with an overbright, doubling the lightmap as they draw it, so 128 is what a surface at
 * full brightness is stored as and 255 is twice as bright as white. ericw-tools is built
 * around that throughout: it calls 128 "the logical value for 1.0 lighting", halves every
 * lightmap through the default "_range" of 0.5, reads "_maxlight" as half the ceiling it
 * clamps to, and multiplies the lightmap by two in its own preview. A preview that
 * divided by 255 would show half the light the game does.
 *
 * Dividing by 128 is that overbright, which is what puts the preview and the compile in
 * the same place.
 */
constexpr auto PreviewLightUnitScale = 1.0f / 128.0f;

/**
 * How much brighter than the stored lightmap the engine draws: the overbright above,
 * written as the factor it multiplies by.
 *
 * "_gamma" is the one thing the compilers apply to the stored lightmap rather than to
 * what is drawn, so dividing it out is what puts a value back on the scale ericw-tools
 * raises to the power.
 */
constexpr auto PreviewOverbright = 255.0f / 128.0f;

/**
 * Turns one traced channel into the 0..255 the preview draws.
 *
 * "_gamma" is why this is not just a multiply: the compilers raise the lightmap they
 * wrote to the power, which is before the engine's overbright rather than after it, so
 * the value goes back onto that scale and comes off it again. Clamping is last for the
 * same reason -- what the compilers clamp is the lightmap, not the light they gathered.
 */
inline uint8_t previewDisplayValue(const float light, const float inverseGamma)
{
  auto value = std::max(light, 0.0f);
  if (inverseGamma != 1.0f)
  {
    value = std::pow(value / PreviewOverbright, inverseGamma) * PreviewOverbright;
  }
  return uint8_t(std::lround(std::min(value, 1.0f) * 255.0f));
}

/**
 * How a surface interacts with rays.
 *
 * Solid surfaces are shaded and block light. NonSolid surfaces are shaded, but light
 * passes through them: this is how water, slime, lava, triggers, clip and hint brushes
 * behave, because the compiler only traces against the solid hull. Sky surfaces are not
 * shaded at all; a ray that hits one leaves the map and picks up the sky radiance
 * instead. Which faces fall into which class is decided by the game config, see
 * LightPreviewScene.cpp.
 */
enum class PreviewSurfaceKind : uint8_t
{
  Solid,
  NonSolid,
  Sky,
};

/**
 * A CPU side copy of a material's texture, used to look up albedo while tracing.
 *
 * The texels are stored at a reduced resolution because the preview is far too noisy for
 * texture detail to survive, and because a full set of mip level 0 images for a large map
 * would be a lot of memory to keep around. If the texture could not be read back, texels
 * is empty and averageColor is used instead.
 */
struct PreviewMaterial
{
  std::string name;
  size_t width = 0;
  size_t height = 0;
  std::vector<vm::vec3f> texels;
  vm::vec3f averageColor = vm::vec3f{0.5f, 0.5f, 0.5f};

  /**
   * Whether any of the texture is see-through, which is what makes it worth asking
   * about a single texel at all. A texture with none is the common case and the tests
   * below are skipped for it.
   */
  bool masked = false;

  /**
   * One byte per texel, zero where the texture is see-through. Only filled in for a
   * masked material.
   */
  std::vector<uint8_t> opaque;

  vm::vec3f sample(const vm::vec2f& uv) const;

  /**
   * Whether the texture has a hole at the given coordinates, where a ray carries on as
   * though the surface were not there.
   */
  bool transparentAt(const vm::vec2f& uv) const;
};

/**
 * The part of a triangle that is only needed once a hit has been found.
 */
struct PreviewTriangleShading
{
  vm::vec3f normal;
  vm::vec2f uv0;
  vm::vec2f uv1;
  vm::vec2f uv2;
  uint32_t materialIndex = 0;
  /**
   * Whether this triangle's texture has holes in it, kept here so that the ray that
   * hits it can tell without reaching for the material first. See
   * PreviewMaterial::transparentAt.
   */
  bool maskedTexture = false;
  PreviewSurfaceKind kind = PreviewSurfaceKind::Solid;
  /** Whether shadow rays are blocked by this triangle. */
  bool occludes = true;
  /**
   * How much of this surface a ray sees, with the rest coming from whatever is behind it.
   * Liquids are drawn partly see-through, so a ray carries on past them even though they
   * are solid enough to be shaded.
   */
  float alpha = 1.0f;
  /** Whether this triangle receives light from light entities (_lightignore). */
  bool receivesLight = true;
  /** Channel mask of the brush model this triangle belongs to (_object_channel_mask). */
  int32_t objectChannelMask = 1;
  /** Radiance emitted by this triangle, for surface lights. */
  vm::vec3f emission = vm::vec3f{0, 0, 0};
  /**
   * How brightly an emitting triangle shows itself, as against how much it gives off.
   * "_surflight_minlight_scale" is what parts the two: a surface can light a room
   * without looking lit itself.
   */
  float selfEmissionScale = 1.0f;
  /**
   * "_surflight_atten": how fast the light off this triangle fades with distance, as a
   * multiple of the distance it is measured over. One is the plain inverse square.
   */
  float emissionAtten = 1.0f;
  /**
   * "_surflightskydist": added to the distance the light off this triangle is measured
   * over, which pushes a sky further away than it really is.
   */
  float emissionDistanceOffset = 0.0f;
  /**
   * Whether this triangle gives off light in every direction rather than out of its own
   * face, which is how the compilers treat a sky lighting the map beneath it.
   */
  bool omnidirectionalEmitter = false;
  /** Per brush model minimum light (_minlight on a bmodel), colour and all. */
  vm::vec3f surfaceMinLight = vm::vec3f{0, 0, 0};
  /** The colour of that minimum light on its own, which its mottle is tinted by. */
  vm::vec3f surfaceMinLightColor = vm::vec3f{1, 1, 1};
  /** Whether this model's minimum light is broken up by noise ("_minlight_mottle"). */
  bool surfaceMinLightMottle = false;
  /** Per brush model ceiling ("_maxlight" on a bmodel); 0 means the map's. */
  float surfaceMaxLight = 0.0f;
  /**
   * "_lightcolorscale" on the brush model: how much colour the light landing on it
   * keeps. One leaves it alone, zero turns it grey.
   */
  float lightColorScale = 1.0f;
  /**
   * Which brush model this triangle belongs to, with zero for the world. Only shadows
   * care: a model can be told to cast one on itself alone, or on the world alone.
   */
  int32_t objectIndex = 0;
  /** "_shadowself": blocks light only where it lands on this same model. */
  bool shadowsSelfOnly = false;
  /** "_shadowworldonly": blocks light only where it lands on the world. */
  bool shadowsWorldOnly = false;
  /** "_dirt" "-1" on the brush model: this surface is never darkened by dirt. */
  bool noDirt = false;
  /**
   * "_phong_angle" on the brush model: how far apart two faces' normals may be and still
   * be smoothed across. Zero, the usual case, means the surface is shaded flat.
   */
  float phongAngle = 0.0f;
  /**
   * Where this triangle's smoothed corner normals are in PreviewScene::smoothNormals, or
   * -1 when it is shaded flat. Kept aside so that a map using no phong pays nothing for
   * the three normals a smoothed triangle needs.
   */
  int32_t smoothIndex = -1;
};

/** The smoothed normals at the three corners of one triangle. See phong shading. */
struct PreviewSmoothNormals
{
  vm::vec3f normal0;
  vm::vec3f normal1;
  vm::vec3f normal2;
};

/**
 * Keeps the albedo of every material the preview has seen, so that a rebuild does not
 * read every texture back from the GPU again.
 *
 * Reading a texture back is a round trip to the driver, and a map can use hundreds of
 * them, which would be a visible stall after every edit. Materials, on the other hand,
 * only change when a collection is reloaded, so the cache survives everything else.
 *
 * The cache must be cleared whenever the material collections change: it is keyed by
 * material address, and a reloaded collection can put a different material where an old
 * one used to be.
 */
class PreviewMaterialCache
{
private:
  std::vector<std::shared_ptr<const PreviewMaterial>> m_materials;
  std::unordered_map<const gl::Material*, uint32_t> m_indices;
  std::unordered_map<std::string, uint32_t> m_indicesByName;

public:
  PreviewMaterialCache();

  /**
   * Returns the index of the given material, reading its albedo back the first time it is
   * seen. A null material maps to index 0, which holds a neutral grey.
   *
   * A material whose texture had not finished uploading when it was first seen keeps its
   * index but is read again on every later call, until it yields something. Caching that
   * first failure permanently is what made a freshly opened map preview untextured until
   * it was opened a second time.
   */
  uint32_t indexOf(const gl::Material* material, gl::Gl& gl, size_t maxTextureSize);

  /** The albedo stored at the given index. */
  const PreviewMaterial& at(uint32_t index) const;

  /** Looks up a material by the name a light entity would refer to it by. */
  std::optional<uint32_t> findByName(const std::string& name) const;

  const std::vector<std::shared_ptr<const PreviewMaterial>>& materials() const;

  void clear();
};

/**
 * An emissive triangle, sampled as an area light. Surface lights are the only emitters
 * that live in the geometry rather than in the light list.
 */
struct PreviewEmitter
{
  uint32_t triangleIndex = 0;
  float area = 0.0f;
  /** Running sum of area over all emitters, used to pick one proportionally to area. */
  float cumulativeArea = 0.0f;
};

/**
 * Everything the tracer needs to render a frame. Immutable once built, so that it can be
 * shared with the worker threads without locking.
 */
struct PreviewScene
{
  std::vector<PreviewTrianglePos> trianglePositions;
  std::vector<PreviewTriangleShading> triangleShading;
  std::vector<std::shared_ptr<const PreviewMaterial>> materials;
  std::vector<PreviewLight> lights;
  /** Smoothed corner normals, indexed by PreviewTriangleShading::smoothIndex. */
  std::vector<PreviewSmoothNormals> smoothNormals;
  std::vector<PreviewEmitter> emitters;
  float totalEmitterArea = 0.0f;

  PreviewBvh bvh;
  vm::bbox3f bounds = vm::bbox3f{vm::vec3f{0, 0, 0}, vm::vec3f{0, 0, 0}};

  PreviewGlobalLighting globals;

  /** Whether the map has any sky face at all; without one there is no sky lighting. */
  bool hasSkyFaces = false;

  bool empty() const { return trianglePositions.empty(); }

  const PreviewTriangleShading& shading(const uint32_t triangleIndex) const
  {
    return triangleShading[triangleIndex];
  }
};

/**
 * What to put in the scene, and how much detail to keep.
 */
struct PreviewSceneOptions
{
  /** The largest side an albedo texture is kept at. */
  size_t maxTextureSize = 64;
  /** Whether entity models are traced along with the brushwork. */
  bool includeEntityModels = false;
};

/**
 * Builds the traceable scene from the given map.
 *
 * Must run on the thread that owns the GL context, because it reads the albedo of each
 * material back from the GPU, and because it walks the map's nodes, which the editor may
 * change from under it otherwise. The returned scene has no hierarchy yet: call
 * buildPreviewSceneBvh on it, which is safe to do from a worker thread since the scene is
 * not shared until then.
 *
 */
PreviewScene buildPreviewScene(
  const mdl::Map& map,
  gl::Gl& gl,
  PreviewMaterialCache& materialCache,
  const PreviewSceneOptions& options);

void buildPreviewSceneBvh(PreviewScene& scene);

} // namespace tb::render
