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

#include "render/LightPreviewLights.h"

#include "mdl/BrushNode.h"
#include "mdl/Entity.h"
#include "mdl/EntityNode.h"
#include "mdl/GroupNode.h"
#include "mdl/LayerNode.h"
#include "mdl/Map.h"
#include "mdl/PatchNode.h"
#include "mdl/WorldNode.h"

#include "kd/overload.h"
#include "kd/string_format.h"
#include "kd/string_utils.h"

#include "vm/scalar.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <initializer_list>
#include <string_view>
#include <unordered_map>

namespace tb::render
{
namespace
{

/**
 * The lightstyle animations Quake ships with. Index 0 is the steady style; the rest are
 * the strings the engine cycles through at ten frames a second, where 'a' is black and
 * 'm' is the light's nominal brightness.
 */
constexpr auto BuiltinLightStyles = std::array<std::string_view, 12>{
  "m",
  "mmnmmommommnonmmonqnmmo",
  "abcdefghijklmnopqrstuvwxyzyxwvutsrqponmlkjihgfedcba",
  "mmmmmaaaaammmmmaaaaaabcdefgabcdefg",
  "mamamamamama",
  "jklmnopqrstuvwxyzyxwvutsrqponmlkj",
  "nmonqnmomnmomomno",
  "mmmaaaabcdefgmmmmaaaammmaamm",
  "mmmaaammmaaammmabcdefaaaammmmabcdefmmmaaaa",
  "aaaaaaaazzzzzzzz",
  "mmamammmmammamamaaamammma",
  "abcdefghijklmnopqrrqponmlkjihgfedcba",
};

bool startsWithIgnoringCase(const std::string_view str, const std::string_view prefix)
{
  return str.size() >= prefix.size()
         && std::equal(
           prefix.begin(), prefix.end(), str.begin(), [](const char a, const char b) {
             return std::tolower(static_cast<unsigned char>(a))
                    == std::tolower(static_cast<unsigned char>(b));
           });
}

/**
 * Scans a property value for numbers, ignoring whatever separates them. Light values come
 * in one, three and four number forms depending on the game and the key, so the parser
 * takes whatever it finds and the caller decides what the count means.
 */
std::vector<float> parseFloats(const std::string& str)
{
  auto result = std::vector<float>{};
  const auto* begin = str.c_str();
  const auto* const end = begin + str.size();

  while (begin != end)
  {
    char* next = nullptr;
    const auto value = std::strtof(begin, &next);
    if (next == begin)
    {
      ++begin;
      continue;
    }
    result.push_back(value);
    begin = next;
  }

  return result;
}

const std::string* findProperty(
  const mdl::Entity& entity, const std::initializer_list<const char*> keys)
{
  for (const auto* key : keys)
  {
    if (const auto* value = entity.property(std::string{key}))
    {
      return value;
    }
  }
  return nullptr;
}

std::optional<float> floatProperty(
  const mdl::Entity& entity, const std::initializer_list<const char*> keys)
{
  if (const auto* value = findProperty(entity, keys))
  {
    const auto numbers = parseFloats(*value);
    if (!numbers.empty())
    {
      return numbers.front();
    }
  }
  return std::nullopt;
}

std::optional<int32_t> intProperty(
  const mdl::Entity& entity, const std::initializer_list<const char*> keys)
{
  if (const auto value = floatProperty(entity, keys))
  {
    return int32_t(*value);
  }
  return std::nullopt;
}

bool flagProperty(
  const mdl::Entity& entity, const std::initializer_list<const char*> keys)
{
  return intProperty(entity, keys).value_or(0) != 0;
}

/**
 * Normalizes a colour written either as three bytes or as three fractions. A component
 * above one can only have been meant as a byte, which is the same rule the compilers use.
 */
vm::vec3f normalizeColor(const vm::vec3f& color)
{
  const auto maximum = std::max({color.x(), color.y(), color.z()});
  const auto result = maximum > 1.0f ? color / 255.0f : color;
  return vm::vec3f{
    std::max(result.x(), 0.0f), std::max(result.y(), 0.0f), std::max(result.z(), 0.0f)};
}

std::optional<vm::vec3f> colorProperty(
  const mdl::Entity& entity, const std::initializer_list<const char*> keys)
{
  if (const auto* value = findProperty(entity, keys))
  {
    const auto numbers = parseFloats(*value);
    if (numbers.size() >= 3)
    {
      return normalizeColor(vm::vec3f{numbers[0], numbers[1], numbers[2]});
    }
    if (numbers.size() == 1)
    {
      return normalizeColor(vm::vec3f{numbers[0], numbers[0], numbers[0]});
    }
  }
  return std::nullopt;
}

/**
 * Turns a yaw and a pitch in degrees into a unit vector, with pitch measured upwards, so
 * that a pitch of -90 points straight down.
 */
vm::vec3f directionFromAngles(const float yaw, const float pitch)
{
  const auto yawRadians = vm::to_radians(yaw);
  const auto pitchRadians = vm::to_radians(pitch);
  const auto cosPitch = std::cos(pitchRadians);
  return vm::vec3f{
    std::cos(yawRadians) * cosPitch,
    std::sin(yawRadians) * cosPitch,
    std::sin(pitchRadians)};
}

/**
 * Reads the direction a light points in.
 *
 * Three spellings have to agree here. "mangle" is Quake's, and lists yaw before pitch.
 * "angles" is GoldSrc's, and lists pitch first. A separate "pitch" key, if present,
 * overrides whichever pitch came out of the other two; GoldSrc uses it because its
 * angles field is unreliable for lights, and it measures pitch upwards, so 90 is up.
 */
std::optional<vm::vec3f> directionFromProperties(const mdl::Entity& entity)
{
  auto yaw = std::optional<float>{};
  auto pitch = std::optional<float>{};

  if (const auto* mangle = findProperty(entity, {"mangle"}))
  {
    const auto numbers = parseFloats(*mangle);
    if (numbers.size() >= 2)
    {
      yaw = numbers[0];
      pitch = numbers[1];
    }
  }

  if (const auto* angles = entity.property("angles"))
  {
    const auto numbers = parseFloats(*angles);
    if (numbers.size() >= 2)
    {
      if (!pitch)
      {
        pitch = numbers[0];
      }
      if (!yaw)
      {
        yaw = numbers[1];
      }
    }
  }

  if (const auto explicitPitch = floatProperty(entity, {"pitch"}))
  {
    pitch = *explicitPitch;
  }

  if (!yaw && !pitch)
  {
    return std::nullopt;
  }

  return directionFromAngles(yaw.value_or(0.0f), pitch.value_or(0.0f));
}

/**
 * The brightness and optional colour of a light entity.
 *
 * ericw-tools reads "light", which is normally a single number but also accepts the
 * Half-Life "r g b n" form. GoldSrc reads "_light", which is "R G B" or "R G B
 * brightness". An explicit "_color" wins over any colour found here.
 */
struct LightValue
{
  float intensity = 300.0f;
  std::optional<vm::vec3f> color;
};

std::optional<LightValue> parseLightValue(const mdl::Entity& entity)
{
  // The two spellings part company over three numbers: GoldSrc's "_light" carries the
  // brightness in the magnitude of the colour, while ericw-tools reads "light" as a
  // colour alone and leaves the brightness at its default.
  const auto* goldSrcValue = entity.property("_light");
  const auto* value = goldSrcValue ? goldSrcValue : findProperty(entity, {"light"});
  if (!value)
  {
    return std::nullopt;
  }

  const auto numbers = parseFloats(*value);
  if (numbers.empty())
  {
    return std::nullopt;
  }

  auto result = LightValue{};
  if (numbers.size() == 1)
  {
    result.intensity = numbers[0];
  }
  else if (numbers.size() == 3)
  {
    result.color = normalizeColor(vm::vec3f{numbers[0], numbers[1], numbers[2]});
    if (goldSrcValue)
    {
      result.intensity = std::max({numbers[0], numbers[1], numbers[2]});
    }
  }
  else
  {
    result.color = normalizeColor(vm::vec3f{numbers[0], numbers[1], numbers[2]});
    result.intensity = numbers[3];
  }

  return result;
}

PreviewAttenuation attenuationFromDelay(const int32_t delay)
{
  switch (delay)
  {
  case 1:
    return PreviewAttenuation::Inverse;
  case 2:
    return PreviewAttenuation::InverseSquare;
  case 3:
    return PreviewAttenuation::None;
  case 4:
    return PreviewAttenuation::LocalMinLight;
  case 5:
    return PreviewAttenuation::InverseSquareOffset;
  default:
    return PreviewAttenuation::Linear;
  }
}

/**
 * The attenuation formula "delay" asks for, which the compilers accept by name as well
 * as by number. An unknown name falls back to linear, as an unknown number does.
 */
std::optional<PreviewAttenuation> attenuationFromDelayProperty(const mdl::Entity& entity)
{
  const auto* value = findProperty(entity, {"delay", "_delay"});
  if (!value)
  {
    return std::nullopt;
  }

  static const auto names = std::unordered_map<std::string, PreviewAttenuation>{
    {"linear", PreviewAttenuation::Linear},
    {"inverse", PreviewAttenuation::Inverse},
    {"inverse2", PreviewAttenuation::InverseSquare},
    {"infinite", PreviewAttenuation::None},
    {"localmin", PreviewAttenuation::LocalMinLight},
    {"inverse2a", PreviewAttenuation::InverseSquareOffset},
  };

  if (const auto it = names.find(kdl::str_to_lower(kdl::str_trim(*value)));
      it != names.end())
  {
    return it->second;
  }

  return attenuationFromDelay(intProperty(entity, {"delay", "_delay"}).value_or(0));
}

using TargetIndex = std::unordered_map<std::string, vm::vec3f>;

/**
 * Points a light at the entity named by its "target" key, if there is one.
 */
std::optional<vm::vec3f> directionFromTarget(
  const mdl::Entity& entity, const vm::vec3f& origin, const TargetIndex& targets)
{
  const auto* target = entity.property("target");
  if (!target || target->empty())
  {
    return std::nullopt;
  }

  const auto it = targets.find(*target);
  if (it == targets.end())
  {
    return std::nullopt;
  }

  const auto delta = it->second - origin;
  if (vm::squared_length(delta) < vm::constants<float>::almost_zero())
  {
    return std::nullopt;
  }

  return vm::normalize(delta);
}

/**
 * Fills in the keys every light shares, whatever kind it ends up being.
 */
void readCommonKeys(
  const mdl::Entity& entity, const PreviewGlobalLighting& globals, PreviewLight& light)
{
  if (const auto value = parseLightValue(entity))
  {
    light.intensity = value->intensity;
    if (value->color)
    {
      light.color = *value->color;
    }
  }

  // "_color" outranks any colour that came out of the light value.
  if (const auto color = colorProperty(entity, {"_color", "color"}))
  {
    light.color = *color;
  }

  // ZHLT's "_fade" is the same idea as ericw-tools' "wait": how quickly the light falls
  // off with distance.
  light.wait = floatProperty(entity, {"wait", "_fade"}).value_or(1.0f);
  if (light.wait <= 0.0f)
  {
    light.wait = 1.0f;
  }

  // GoldSrc spells the brightness "_light" where ericw-tools spells it "light", and its
  // compiler falls off with the square of the distance where ericw's falls off linearly.
  // An entity written the GoldSrc way, and not saying otherwise, is previewed the GoldSrc
  // way: reading it with ericw's defaults would wash a Half-Life map out completely.
  //
  // NOTE: this is inferred from the key rather than from the game, which is the sort of
  // thing that should come from the game configuration once it can say so.
  const auto goldSrc = entity.property("_light") != nullptr;
  light.attenuation = attenuationFromDelayProperty(entity).value_or(
    goldSrc ? PreviewAttenuation::InverseSquare : PreviewAttenuation::Linear);
  light.falloff = std::max(floatProperty(entity, {"_falloff"}).value_or(0.0f), 0.0f);
  // An "_anglescale" outside the range it is defined over means "use the map's", which
  // is how a light says so: the compilers read -1 that way rather than clamping it to a
  // surface the angle has no effect on at all.
  const auto entityAngleScale = floatProperty(entity, {"_anglescale", "_anglesense"});
  light.angleScale =
    entityAngleScale && *entityAngleScale >= 0.0f && *entityAngleScale <= 1.0f
      ? *entityAngleScale
      : globals.defaultAngleScale;

  light.bleed = flagProperty(entity, {"_bleed"});

  light.deviance = std::max(floatProperty(entity, {"_deviance"}).value_or(0.0f), 0.0f);
  light.devianceSamples =
    std::clamp(intProperty(entity, {"_samples"}).value_or(16), 1, 128);

  light.bounceScale = floatProperty(entity, {"_bouncescale"}).value_or(1.0f);

  light.dirt = intProperty(entity, {"_dirt"}).value_or(0);
  light.dirtScale = std::max(floatProperty(entity, {"_dirtscale"}).value_or(0.0f), 0.0f);
  light.dirtGain = std::max(floatProperty(entity, {"_dirtgain"}).value_or(0.0f), 0.0f);

  const auto dirtOff = floatProperty(entity, {"_dirt_off_radius"});
  const auto dirtOn = floatProperty(entity, {"_dirt_on_radius"});
  light.dirtRadiusSet = dirtOff.has_value() && dirtOn.has_value();
  light.dirtOffRadius = dirtOff.value_or(0.0f);
  light.dirtOnRadius = dirtOn.value_or(0.0f);

  light.style = intProperty(entity, {"style"}).value_or(0);
  light.styleScale =
    averageLightStyleBrightness(light.style, findProperty(entity, {"pattern"}));

  light.lightChannelMask = intProperty(entity, {"_light_channel_mask"}).value_or(1);
  light.shadowChannelMask =
    intProperty(entity, {"_shadow_channel_mask"}).value_or(light.lightChannelMask);

  if (light.attenuation == PreviewAttenuation::LocalMinLight)
  {
    light.kind = PreviewLightKind::LocalMinLight;
  }
}

/**
 * Reads the cone of a spotlight.
 *
 * The two spellings do not measure the same thing. GoldSrc's "_cone2" is the half angle
 * out to the edge of the cone and "_cone" the half angle of the hot spot inside it.
 * ericw-tools' "angle" is the width of the whole cone and "_softangle" the width of the
 * hot spot, so half of each is the angle from the axis that the light reaches --
 * reading them as half angles gives a cone twice as wide as the compiler's.
 */
void readSpotCone(const mdl::Entity& entity, PreviewLight& light)
{
  const auto halfWidth =
    [&](const std::initializer_list<const char*> keys, const float fallback) {
      return floatProperty(entity, keys).value_or(fallback) * 0.5f;
    };

  const auto outer =
    floatProperty(entity, {"_cone2"}).value_or(halfWidth({"angle", "_angle"}, 40.0f));
  const auto inner = floatProperty(entity, {"_cone"})
                       .value_or(halfWidth({"_softangle", "softangle"}, 0.0f));

  const auto clampedOuter = std::clamp(outer, 0.5f, 179.0f);
  light.cosOuterCone = std::cos(vm::to_radians(clampedOuter));
  // A hot spot that is missing, or as wide as the cone or wider, means a hard edge.
  light.cosInnerCone = inner > 0.0f && inner < clampedOuter
                         ? std::cos(vm::to_radians(inner))
                         : light.cosOuterCone;
  light.spot = true;
}

/**
 * Reads a projected texture light, the gobo that a light shines through.
 *
 * "_project_mangle" aims the projection, overriding "mangle". Since the frustum the gobo
 * is projected through is also what bounds the light, aiming the projection aims the
 * light with it.
 */
void readProjectedTexture(const mdl::Entity& entity, PreviewLight& light)
{
  const auto* texture = entity.property("_project_texture");
  if (!texture || texture->empty())
  {
    return;
  }

  light.projectedTextureName = *texture;

  const auto fov =
    std::clamp(floatProperty(entity, {"_project_fov"}).value_or(90.0f), 1.0f, 179.0f);
  light.projectTanHalfFov = std::tan(vm::to_radians(fov) / 2.0f);

  if (const auto* mangle = entity.property("_project_mangle"))
  {
    const auto numbers = parseFloats(*mangle);
    if (numbers.size() >= 2)
    {
      light.direction = directionFromAngles(numbers[0], numbers[1]);
    }
  }

  light.spot = true;

  // Build the frame the projection is unwrapped in. Any up vector that is not parallel to
  // the projection direction will do, since a gobo has no inherent orientation.
  const auto reference =
    std::abs(light.direction.z()) > 0.99f ? vm::vec3f{1, 0, 0} : vm::vec3f{0, 0, 1};
  light.projectRight = vm::normalize(vm::cross(light.direction, reference));
  light.projectUp = vm::normalize(vm::cross(light.projectRight, light.direction));
}

PreviewGlobalLighting parseGlobals(const mdl::Entity& worldspawn)
{
  auto result = PreviewGlobalLighting{};

  const auto skyDomeIntensity = floatProperty(worldspawn, {"_sunlight2"}).value_or(0.0f);
  const auto skyDomeColor =
    colorProperty(worldspawn, {"_sunlight2_color", "_sunlight_color2"})
      .value_or(vm::vec3f{1, 1, 1});
  result.skyDome = skyDomeColor * skyDomeIntensity;

  const auto groundDomeIntensity =
    floatProperty(worldspawn, {"_sunlight3"}).value_or(0.0f);
  const auto groundDomeColor =
    colorProperty(worldspawn, {"_sunlight3_color", "_sunlight_color3"})
      .value_or(vm::vec3f{1, 1, 1});
  result.groundDome = groundDomeColor * groundDomeIntensity;

  // "light" on worldspawn is the legacy spelling of "_minlight".
  const auto minLightIntensity =
    floatProperty(worldspawn, {"_minlight", "light"}).value_or(0.0f);
  const auto minLightColor = colorProperty(worldspawn, {"_minlight_color", "_mincolor"})
                               .value_or(vm::vec3f{1, 1, 1});
  result.minLight = minLightColor * minLightIntensity;

  result.addMinLight = flagProperty(worldspawn, {"_addmin"});

  result.dirt = flagProperty(worldspawn, {"_dirt", "_dirty"});
  result.dirtMode = intProperty(worldspawn, {"_dirtmode"}).value_or(0);
  result.dirtDepth =
    std::max(floatProperty(worldspawn, {"_dirtdepth"}).value_or(128.0f), 1.0f);
  result.dirtScale =
    std::clamp(floatProperty(worldspawn, {"_dirtscale"}).value_or(1.0f), 0.0f, 100.0f);
  result.dirtGain =
    std::clamp(floatProperty(worldspawn, {"_dirtgain"}).value_or(1.0f), 0.0f, 100.0f);
  result.dirtAngle =
    std::clamp(floatProperty(worldspawn, {"_dirtangle"}).value_or(88.0f), 1.0f, 90.0f);
  result.minLightDirt = flagProperty(worldspawn, {"_minlight_dirt"});
  result.distScale = std::max(floatProperty(worldspawn, {"_dist"}).value_or(1.0f), 0.0f);
  // ericw-tools halves every lightmap unless the map says otherwise, so a preview that
  // leaves it at one is twice as bright as the compile it is standing in for.
  result.rangeScale =
    std::max(floatProperty(worldspawn, {"_range"}).value_or(DefaultRangeScale), 0.0f);
  result.gamma =
    std::clamp(floatProperty(worldspawn, {"_gamma"}).value_or(1.0f), 0.1f, 5.0f);
  result.maxLight =
    std::max(floatProperty(worldspawn, {"_maxlight"}).value_or(0.0f), 0.0f);
  result.defaultAngleScale = std::clamp(
    floatProperty(worldspawn, {"_anglescale", "_anglesense"}).value_or(0.5f), 0.0f, 1.0f);

  // The compilers do not bounce light unless they are asked to, and when they do, the
  // bounce ignores the colour of the surface it came off unless that is asked for too.
  // "_bounce" is a count: how many times light is allowed to bounce. Clamped because the
  // preview pays for every extra bounce on every pass, where the compiler pays once.
  result.bounces =
    std::clamp(intProperty(worldspawn, {"_bounce"}).value_or(0), 0, MaxPreviewBounces);
  result.bounceScale =
    std::max(floatProperty(worldspawn, {"_bouncescale"}).value_or(1.0f), 0.0f);
  result.bounceColorScale = std::clamp(
    floatProperty(worldspawn, {"_bouncecolorscale"}).value_or(0.0f), 0.0f, 1.0f);

  result.surfaceLightScale =
    std::max(floatProperty(worldspawn, {"_surflightscale"}).value_or(1.0f), 0.0f);
  result.surfaceSkyLightScale =
    std::max(floatProperty(worldspawn, {"_surflightskyscale"}).value_or(1.0f), 0.0f);

  return result;
}

/**
 * Adds the suns configured on worldspawn: "_sunlight" and the independent second sun
 * "_sun2". Neither has a position; only the direction they shine in matters.
 */
void addWorldspawnSuns(const mdl::Entity& worldspawn, PreviewLighting& lighting)
{
  const auto addSun = [&](
                        const std::initializer_list<const char*> intensityKeys,
                        const std::initializer_list<const char*> colorKeys,
                        const std::initializer_list<const char*> mangleKeys,
                        const float penumbra) {
    const auto intensity = floatProperty(worldspawn, intensityKeys).value_or(0.0f);
    if (intensity <= 0.0f)
    {
      return;
    }

    auto light = PreviewLight{};
    light.kind = PreviewLightKind::Sun;
    light.intensity = intensity;
    light.color = colorProperty(worldspawn, colorKeys).value_or(vm::vec3f{1, 1, 1});
    light.attenuation = PreviewAttenuation::None;
    light.angleScale = floatProperty(worldspawn, {"_anglescale", "_anglesense"})
                         .value_or(lighting.globals.defaultAngleScale);
    light.penumbra = penumbra;

    auto direction = vm::vec3f{0, 0, -1};
    if (const auto* mangle = findProperty(worldspawn, mangleKeys))
    {
      const auto numbers = parseFloats(*mangle);
      if (numbers.size() >= 2)
      {
        direction = directionFromAngles(numbers[0], numbers[1]);
      }
    }
    light.direction = direction;

    lighting.lights.push_back(light);
  };

  const auto penumbra =
    std::max(floatProperty(worldspawn, {"_sunlight_penumbra"}).value_or(0.0f), 0.0f);

  addSun(
    {"_sunlight", "_sun_light"},
    {"_sunlight_color", "_sun_color"},
    {"_sunlight_mangle", "_sun_mangle", "_sun_angle"},
    penumbra);
  addSun({"_sun2"}, {"_sun2_color"}, {"_sun2_mangle"}, penumbra);
}

/**
 * Handles the light entities that configure something else instead of emitting light
 * themselves. Returns true if the entity was one of them and has been dealt with.
 */
bool parseConfigCarrier(
  const mdl::Entity& entity,
  const vm::vec3f& origin,
  const TargetIndex& targets,
  PreviewLighting& lighting)
{
  // "_sunlight2" and "_sunlight3" on a light entity are another way of writing the sky
  // and ground domes. The entity itself is disabled and can sit anywhere.
  const auto addDome = [&](vm::vec3f& dome) {
    const auto value = parseLightValue(entity);
    const auto intensity = value ? value->intensity : 0.0f;
    const auto color =
      colorProperty(entity, {"_color", "color"})
        .value_or(value && value->color ? *value->color : vm::vec3f{1, 1, 1});
    dome = dome + color * intensity;
  };

  if (flagProperty(entity, {"_sunlight2"}))
  {
    addDome(lighting.globals.skyDome);
    return true;
  }

  if (flagProperty(entity, {"_sunlight3"}))
  {
    addDome(lighting.globals.groundDome);
    return true;
  }

  if (flagProperty(entity, {"_sun"}))
  {
    auto light = PreviewLight{};
    light.kind = PreviewLightKind::Sun;
    light.attenuation = PreviewAttenuation::None;
    readCommonKeys(entity, lighting.globals, light);
    light.kind = PreviewLightKind::Sun;
    light.attenuation = PreviewAttenuation::None;

    // On a sun entity the penumbra is spelled "deviance", without the underscore that
    // the same idea uses on a point light.
    light.penumbra = std::max(floatProperty(entity, {"deviance"}).value_or(0.0f), 0.0f);
    light.deviance = 0.0f;

    if (const auto direction = directionFromTarget(entity, origin, targets))
    {
      light.direction = *direction;
    }
    else if (const auto direction = directionFromProperties(entity))
    {
      light.direction = *direction;
    }
    else
    {
      light.direction = vm::vec3f{0, 0, -1};
    }

    if (light.intensity > 0.0f)
    {
      lighting.lights.push_back(light);
    }
    return true;
  }

  return false;
}

/**
 * Reads an ericw-tools surface light template: a light entity whose "_surface" key names
 * a texture, turning every face that uses it into an emitter.
 */
void parseSurfaceLight(
  const mdl::Entity& entity, const std::string& materialName, PreviewLighting& lighting)
{
  auto surfaceLight = PreviewSurfaceLightTemplate{};
  surfaceLight.materialName = materialName;

  if (const auto value = parseLightValue(entity))
  {
    surfaceLight.intensity = value->intensity;
    if (value->color)
    {
      surfaceLight.color = *value->color;
    }
  }
  if (const auto color = colorProperty(entity, {"_color", "color"}))
  {
    surfaceLight.color = *color;
  }

  surfaceLight.offset = floatProperty(entity, {"_surface_offset"}).value_or(2.0f);
  surfaceLight.spotlight = flagProperty(entity, {"_surface_spotlight"});
  surfaceLight.styleScale = averageLightStyleBrightness(
    intProperty(entity, {"style"}).value_or(0), findProperty(entity, {"pattern"}));
  surfaceLight.lightChannelMask =
    intProperty(entity, {"_light_channel_mask"}).value_or(1);
  if (const auto* group = findProperty(entity, {"_surflight_group"}))
  {
    surfaceLight.group = *group;
  }

  lighting.surfaceLights.push_back(std::move(surfaceLight));
}

/**
 * Reads the ZHLT/VHLT info_texlights entity, whose properties are texture names mapped to
 * "R G B brightness" values. It is the entity form of a lights.rad file.
 */
void parseTexLights(const mdl::Entity& entity, PreviewLighting& lighting)
{
  for (const auto& property : entity.properties())
  {
    const auto& key = property.key();
    if (key == "classname" || key == "origin" || key.empty() || key.front() == '_')
    {
      continue;
    }

    const auto numbers = parseFloats(property.value());
    if (numbers.size() < 3)
    {
      continue;
    }

    auto surfaceLight = PreviewSurfaceLightTemplate{};
    surfaceLight.materialName = key;
    surfaceLight.color = normalizeColor(vm::vec3f{numbers[0], numbers[1], numbers[2]});
    surfaceLight.intensity =
      numbers.size() >= 4 ? numbers[3] : std::max({numbers[0], numbers[1], numbers[2]});
    lighting.surfaceLights.push_back(std::move(surfaceLight));
  }
}

void parseLightEntity(
  const mdl::Entity& entity, const TargetIndex& targets, PreviewLighting& lighting)
{
  const auto origin = vm::vec3f{entity.origin()};

  // "_nostaticlight" tells the compiler to skip the entity entirely.
  if (flagProperty(entity, {"_nostaticlight"}))
  {
    return;
  }

  if (parseConfigCarrier(entity, origin, targets, lighting))
  {
    return;
  }

  if (const auto* surface = findProperty(entity, {"_surface", "_surflight_texture"}))
  {
    if (!surface->empty())
    {
      parseSurfaceLight(entity, *surface, lighting);
      return;
    }
  }

  // GoldSrc's light_environment is a sun, and so is a light_spot with "_sky" set.
  const auto isEnvironment =
    startsWithIgnoringCase(entity.classname(), "light_environment")
    || flagProperty(entity, {"_sky"});

  auto light = PreviewLight{};
  readCommonKeys(entity, lighting.globals, light);
  light.origin = origin;

  if (isEnvironment)
  {
    light.kind = PreviewLightKind::Sun;
    light.attenuation = PreviewAttenuation::None;
    light.direction =
      directionFromTarget(entity, origin, targets)
        .value_or(directionFromProperties(entity).value_or(vm::vec3f{0, 0, -1}));
    light.penumbra = std::max(
      floatProperty(entity, {"_spread", "_sunlight_penumbra", "deviance"}).value_or(0.0f),
      0.0f);

    // ZHLT's "_diffuse_light" is the ambient half of a light_environment: light from the
    // whole sky rather than from the sun's direction.
    const auto diffuse =
      floatProperty(entity, {"_diffuse_light", "_diffuse_light2"}).value_or(0.0f);
    if (diffuse > 0.0f)
    {
      lighting.globals.skyDome = lighting.globals.skyDome + light.color * diffuse;
    }
  }
  else
  {
    // A light becomes a spotlight as soon as it is aimed, whether by targeting an entity
    // or by an explicit angle. This is why GoldSrc's light and light_spot are the same
    // class as far as the game is concerned.
    // A light becomes a spotlight when it is aimed, but "angles" alone does not count:
    // editors write "angles" "0 0 0" on all sorts of point entities, and treating that as
    // aim would turn every such light into a spot pointing east.
    const auto targetDirection = directionFromTarget(entity, origin, targets);
    const auto hasCone =
      findProperty(entity, {"_cone", "_cone2", "_softangle", "softangle"}) != nullptr
      || startsWithIgnoringCase(entity.classname(), "light_spot");
    const auto aimed = hasCone || findProperty(entity, {"mangle", "pitch"}) != nullptr;
    const auto angleDirection = aimed ? directionFromProperties(entity) : std::nullopt;

    if (targetDirection || angleDirection || hasCone)
    {
      light.kind = PreviewLightKind::Spot;
      light.direction =
        targetDirection.value_or(angleDirection.value_or(vm::vec3f{0, 0, -1}));
      readSpotCone(entity, light);

      // "_spotlightautofalloff" makes the cone reach exactly as far as the entity it
      // aims at, unless an explicit falloff already says otherwise.
      if (light.falloff <= 0.0f && targetDirection)
      {
        if (const auto* target = entity.property("target"))
        {
          const auto it = targets.find(*target);
          if (it != targets.end() && flagProperty(entity, {"_spotlightautofalloff"}))
          {
            light.falloff = vm::length(it->second - origin);
          }
        }
      }
    }

    readProjectedTexture(entity, light);
  }

  if (light.attenuation == PreviewAttenuation::LocalMinLight)
  {
    light.kind = PreviewLightKind::LocalMinLight;
  }

  if (light.intensity == 0.0f)
  {
    return;
  }

  lighting.lights.push_back(std::move(light));
}

} // namespace

float averageLightStyleBrightness(const int32_t style, const std::string* pattern)
{
  const auto averageOf = [](const std::string_view str) {
    if (str.empty())
    {
      return 1.0f;
    }

    auto sum = 0.0f;
    for (const auto c : str)
    {
      const auto lower = char(std::tolower(static_cast<unsigned char>(c)));
      if (lower < 'a' || lower > 'z')
      {
        continue;
      }
      sum += float(lower - 'a') / float('m' - 'a');
    }
    return sum / float(str.size());
  };

  if (pattern && !pattern->empty())
  {
    return averageOf(*pattern);
  }

  if (style <= 0)
  {
    return 1.0f;
  }

  if (size_t(style) < BuiltinLightStyles.size())
  {
    return averageOf(BuiltinLightStyles[size_t(style)]);
  }

  // Styles beyond the built in set are switchable ones the compiler assigns; previewing
  // them lit is more useful than previewing them dark.
  return 1.0f;
}

PreviewLighting extractLighting(const mdl::Map& map)
{
  auto result = PreviewLighting{};

  const auto& worldNode = map.worldNode();
  const auto& worldspawn = worldNode.entity();

  result.globals = parseGlobals(worldspawn);
  addWorldspawnSuns(worldspawn, result);

  auto entities = std::vector<const mdl::Entity*>{};
  auto targets = TargetIndex{};

  const auto collect = [&](const mdl::EntityNode& node) {
    const auto& entity = node.entity();
    entities.push_back(&entity);

    if (const auto* targetname = entity.property("targetname"))
    {
      // A brush entity has no origin key, so fall back to the centre of its bounds; that
      // is where a light aimed at it should point.
      const auto position = entity.property("origin")
                              ? vm::vec3f{entity.origin()}
                              : vm::vec3f{node.logicalBounds().center()};
      targets.emplace(*targetname, position);
    }
  };

  worldNode.accept(kdl::overload(
    [](auto&& thisLambda, const mdl::WorldNode& node) { node.visitChildren(thisLambda); },
    [](auto&& thisLambda, const mdl::LayerNode& node) { node.visitChildren(thisLambda); },
    [](auto&& thisLambda, const mdl::GroupNode& node) { node.visitChildren(thisLambda); },
    [&](auto&& thisLambda, const mdl::EntityNode& node) {
      collect(node);
      node.visitChildren(thisLambda);
    },
    [](const mdl::BrushNode&) {},
    [](const mdl::PatchNode&) {}));

  for (const auto* entity : entities)
  {
    const auto& classname = entity->classname();

    // ericw-tools applies the light keys to any classname that starts with "light", so
    // that a mod's light_globe or light_flame_small_yellow keeps working. Matching the
    // prefix rather than the exact name is what keeps those lights from silently losing
    // their properties.
    if (startsWithIgnoringCase(classname, "light"))
    {
      parseLightEntity(*entity, targets, result);
    }
    else if (startsWithIgnoringCase(classname, "info_texlights"))
    {
      parseTexLights(*entity, result);
    }
  }

  // Dirt costs a sheaf of rays at every point that is shaded, so nothing pays for it
  // unless something in the map has asked for it: the map as a whole, its minimum light,
  // a sun, or any one light.
  result.globals.dirtInUse = result.globals.dirt || result.globals.minLightDirt
                             || std::ranges::any_of(result.lights, [](const auto& light) {
                                  return light.dirt > 0;
                                });

  return result;
}

} // namespace tb::render
