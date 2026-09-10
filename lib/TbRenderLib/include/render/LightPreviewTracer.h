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

#include "vm/vec.h"

#include <cstdint>

namespace tb::gl
{
class Camera;
}

namespace tb::render
{

struct PreviewScene;

/**
 * A copy of everything about the camera that generating rays needs.
 *
 * The worker threads must not touch the live camera: it caches its matrices lazily, and
 * the editor may move it at any moment. Taking a snapshot on the render thread also gives
 * the preview something to compare against to notice that the view has moved.
 */
struct PreviewCamera
{
  vm::vec3f position = vm::vec3f{0, 0, 0};
  vm::vec3f forward = vm::vec3f{1, 0, 0};
  vm::vec3f right = vm::vec3f{0, -1, 0};
  vm::vec3f up = vm::vec3f{0, 0, 1};
  /** Half the width and height of the view plane at unit distance. */
  float halfWidth = 1.0f;
  float halfHeight = 1.0f;
  int width = 1;
  int height = 1;

  /**
   * The direction of the ray through the given point in pixels, measured from the top
   * left corner of the viewport.
   */
  vm::vec3f rayDirection(float x, float y) const;

  bool operator==(const PreviewCamera& other) const;
  bool operator!=(const PreviewCamera& other) const { return !(*this == other); }
};

/**
 * Builds a snapshot of the given camera at the given render resolution.
 */
PreviewCamera makePreviewCamera(const gl::Camera& camera, int width, int height);

/**
 * Whether the preview computes indirect light, and whether the map gets to decide.
 *
 * The compilers do not bounce light unless "_bounce" asks them to, so following the map
 * means most maps preview without it. Overriding is the preview's equivalent of passing
 * "-bounce" on the command line, which is how a mapper sees what it would do before
 * committing the key.
 */
enum class PreviewIndirectLight
{
  FromMap,
  On,
  Off,
};

struct PreviewTraceSettings
{
  /** Whether indirect light is computed, overriding the map's "_bounce" key. */
  PreviewIndirectLight indirectLight = PreviewIndirectLight::FromMap;

  /** How many times a ray may bounce after the surface the camera sees. */
  int32_t maxBounces = 2;
  /** The largest number of shadow rays one shading point may spend on light entities. */
  int32_t maxShadowRays = 8;
  /** Scales the final image, for previews that come out too dark or too bright. */
  float exposure = 1.0f;
};

/**
 * Traces one path through the given pixel and returns its radiance, where 1 is a fully
 * lit white surface.
 *
 * sampleIndex seeds the random numbers, so that the same pixel takes a different path
 * every pass and the passes average out to the converged image.
 */
vm::vec3f tracePreviewPixel(
  const PreviewScene& scene,
  const PreviewCamera& camera,
  const PreviewTraceSettings& settings,
  int32_t x,
  int32_t y,
  uint32_t sampleIndex);

} // namespace tb::render
