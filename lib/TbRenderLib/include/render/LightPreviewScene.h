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
 * Both the "light" key and the lightmap the compilers write run to 255 for a fully lit
 * surface, so dividing by 255 at the end of shading puts the preview in display range.
 */
constexpr auto PreviewLightUnitScale = 1.0f / 255.0f;

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
  /** Per brush model minimum light (_minlight on a bmodel), premultiplied by nothing. */
  vm::vec3f surfaceMinLight = vm::vec3f{0, 0, 0};
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
