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
#include <string>
#include <vector>

namespace tb::mdl
{
class Map;
class Entity;
} // namespace tb::mdl

namespace tb::render
{

/**
 * The normalized light types the preview understands.
 *
 * The many entity spellings a mapper can use (a Quake light, an ericw-tools "_sun" "1"
 * config carrier, a GoldSrc light_environment, ...) all collapse into one of these before
 * the tracer sees them. Surface emitters are missing on purpose: they live on the
 * geometry, as emissive triangles, not in the light list.
 */
enum class PreviewLightKind : uint8_t
{
  Point,
  Spot,
  /** An infinitely distant directional light emitted through sky faces. */
  Sun,
  /** delay 4: no attenuation, non additive, limited to what the entity can see. */
  LocalMinLight,
};

/**
 * The attenuation formulas of the "delay" key, with the same numbering the key uses.
 */
enum class PreviewAttenuation : uint8_t
{
  Linear = 0,
  Inverse = 1,
  InverseSquare = 2,
  None = 3,
  LocalMinLight = 4,
  /**
   * Inverse square offset by the scale constant, which removes the blown out highlight
   * that delay 2 produces close to the source.
   */
  InverseSquareOffset = 5,
};

/**
 * A light, normalized away from whichever entity form the mapper wrote it in.
 */
struct PreviewLight
{
  PreviewLightKind kind = PreviewLightKind::Point;

  vm::vec3f origin = vm::vec3f{0, 0, 0};
  /** The direction the light travels in, for spots and suns. */
  vm::vec3f direction = vm::vec3f{0, 0, -1};

  /** Colour in 0..1, already normalized from whichever 0..255 form was written. */
  vm::vec3f color = vm::vec3f{1, 1, 1};
  /** The "light" key: brightness before attenuation. May be negative to subtract light.
   */
  float intensity = 300.0f;

  PreviewAttenuation attenuation = PreviewAttenuation::Linear;
  /** The "wait" key: scales how quickly the light fades with distance. */
  float wait = 1.0f;
  /** The "_falloff" key: distance at which the light reaches zero. 0 means unused. */
  float falloff = 0.0f;

  /** The "_anglescale" key: how much the angle of incidence matters, 0..1. */
  float angleScale = 0.5f;

  bool spot = false;
  /** Cosine of the half angle where the cone ends. */
  float cosOuterCone = -1.0f;
  /** Cosine of the half angle where the cone starts to fade. */
  float cosInnerCone = -1.0f;

  /** The "_deviance" key: radius of the sphere the light is spread over. */
  float deviance = 0.0f;
  /** The "_samples" key: how many lights _deviance splits this one into. */
  int32_t devianceSamples = 16;

  /** Half angle of the sun's disc in degrees, from "_sunlight_penumbra"/"deviance". */
  float penumbra = 0.0f;

  /** Average brightness of the light's style, so animated lights preview steadily. */
  float styleScale = 1.0f;
  int32_t style = 0;

  /** The "_bouncescale" key: how much this light contributes to indirect light. */
  float bounceScale = 1.0f;

  /** The "_light_channel_mask" key, ANDed with a surface's object channel mask. */
  int32_t lightChannelMask = 1;
  /** The "_shadow_channel_mask" key; defaults to the light channel mask. */
  int32_t shadowChannelMask = 1;

  /** The "_project_texture" key: a gobo the light projects. Empty if there is none. */
  std::string projectedTextureName;
  /** Index into PreviewScene::materials of the gobo, resolved by the scene builder. */
  int32_t projectedTextureIndex = -1;
  vm::vec3f projectRight = vm::vec3f{0, 1, 0};
  vm::vec3f projectUp = vm::vec3f{0, 0, 1};
  float projectTanHalfFov = 1.0f;

  /** Set for lights the compiler would not bake, so the UI can report them. */
  bool enabled = true;
};

/**
 * A light entity carrying "_surface", which turns every face using the named texture into
 * an emitter rather than emitting anything itself.
 */
struct PreviewSurfaceLightTemplate
{
  std::string materialName;
  vm::vec3f color = vm::vec3f{1, 1, 1};
  float intensity = 300.0f;
  float offset = 2.0f;
  float styleScale = 1.0f;
  int32_t lightChannelMask = 1;
  bool spotlight = false;
  std::string group;
};

/**
 * Lighting that has no position: the sky domes, the global minimum light, and the knobs
 * that scale every light in the map.
 */
struct PreviewGlobalLighting
{
  /** "_sunlight2": diffuse light from the upper hemisphere, emitted by sky faces. */
  vm::vec3f skyDome = vm::vec3f{0, 0, 0};
  /** "_sunlight3": the same for the lower hemisphere. */
  vm::vec3f groundDome = vm::vec3f{0, 0, 0};

  /** "_minlight": a floor on the light every surface receives. */
  vm::vec3f minLight = vm::vec3f{0, 0, 0};

  /** "_dist": scales the fade distance of every light. */
  float distScale = 1.0f;
  /** "_range": scales the brightness of every light without changing its reach. */
  float rangeScale = 1.0f;
  /** "_gamma": applied to the final image. */
  float gamma = 1.0f;
  /** "_maxlight": upper clamp, 0 means unclamped. */
  float maxLight = 0.0f;

  /** "_anglescale": the default angle of incidence response for lights that omit it. */
  float defaultAngleScale = 0.5f;

  /** "_bounce": whether indirect light is computed at all. Off unless asked for. */
  bool bounceEnabled = false;
  /** "_bouncescale": how strong indirect light is. */
  float bounceScale = 1.0f;
  /** "_bouncecolorscale": how much indirect light picks up the colour of surfaces. */
  float bounceColorScale = 0.0f;

  /** "_surflightscale" and "_surflightskyscale". */
  float surfaceLightScale = 1.0f;
  float surfaceSkyLightScale = 1.0f;
};

/**
 * Everything parsed out of the map's entities, before it is attached to geometry.
 */
struct PreviewLighting
{
  std::vector<PreviewLight> lights;
  std::vector<PreviewSurfaceLightTemplate> surfaceLights;
  PreviewGlobalLighting globals;
  /** Notes about keys that were recognized but are not previewed faithfully. */
  std::vector<std::string> notes;
};

/**
 * Returns the average brightness of a light style, where 1.0 is a steady light.
 *
 * Animated styles are previewed at their average rather than at whatever frame the clock
 * happens to be on, so that a flickering torch does not make the preview flicker too.
 */
float averageLightStyleBrightness(int32_t style, const std::string* pattern);

/**
 * Collects the lighting of the given map.
 *
 * projectedTextureIndex on the returned lights is left at -1; the scene builder resolves
 * gobo textures once it knows which materials it loaded.
 */
PreviewLighting extractLighting(const mdl::Map& map);

} // namespace tb::render
