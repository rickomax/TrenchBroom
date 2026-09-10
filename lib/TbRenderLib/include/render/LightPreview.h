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

#include "mdl/BrushRendererBrushCache.h"

#include "vm/vec.h"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace tb::mdl
{
class BrushFace;
class Map;
class PatchNode;
} // namespace tb::mdl

namespace tb::render
{

/**
 * A rough preview of what the map's lights will do, computed on the CPU and baked into
 * the vertex colors of brushes and patches.
 *
 * The lights are the map's light entities and any face whose material carries the
 * surface light flag. Each vertex is lit by every light that reaches it, with the light
 * entity's falloff, its style's current brightness, and a ray cast back to the light to
 * leave the vertex dark when something is in the way. This is nothing like what a real
 * compiler produces, but it is enough to see where a room will be lit and where it will
 * not without compiling.
 *
 * A preview is built for one frame and thrown away; its revision changes whenever the
 * map or the style animation would give a different result, which is what tells the
 * renderers' caches to rebuild.
 */
class LightPreview : public mdl::VertexLighting
{
public:
  struct Light
  {
    vm::vec3f position;
    /** The face normal for a surface light, which only lights what is in front of it. */
    vm::vec3f direction;
    vm::vec3f color;
    float intensity = 0.0f;
    float radius = 0.0f;
    int style = 0;
    /** The light entity's "delay" key: the shape of its falloff curve. */
    int falloff = 0;
    bool isSurface = false;
  };

private:
  mdl::Map& m_map;
  std::vector<Light> m_lights;
  vm::vec3f m_ambient;
  uint64_t m_revision = 0;
  uint32_t m_styleFrame = 0;
  std::unordered_map<int, std::string> m_stylePatterns;

public:
  /** Collects the map's lights. The time drives the animated light styles. */
  LightPreview(mdl::Map& map, float timeSeconds);

  const std::vector<Light>& lights() const;
  const vm::vec3f& ambient() const;

  uint64_t revision() const override;

  vm::vec3f lightingAt(
    const vm::vec3f& position,
    const vm::vec3f& normal,
    const mdl::BrushFace* face) const override;

  /**
   * The light reaching the given point. The face or patch the point belongs to is
   * ignored when looking for what blocks the light, so that a surface does not shadow
   * itself.
   */
  vm::vec3f lightingAt(
    const vm::vec3f& position,
    const vm::vec3f& normal,
    const mdl::BrushFace* ignoreFace,
    const mdl::PatchNode* ignorePatch) const;

private:
  void collectStylePatterns();
  void collectPointLights();
  void collectSurfaceLights();
  float styleIntensity(int style) const;
  bool isOccluded(
    const vm::vec3f& from,
    const vm::vec3f& to,
    const mdl::BrushFace* ignoreFace,
    const mdl::PatchNode* ignorePatch) const;
};

} // namespace tb::render
