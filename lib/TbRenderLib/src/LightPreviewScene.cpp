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

#include "render/LightPreviewScene.h"

#include "gl/GlInterface.h"
#include "gl/Material.h"
#include "gl/Texture.h"
#include "mdl/Brush.h"
#include "mdl/BrushFace.h"
#include "mdl/BrushNode.h"
#include "mdl/EditorContext.h"
#include "mdl/Entity.h"
#include "mdl/EntityNode.h"
#include "mdl/EntityNodeBase.h"
#include "mdl/GameConfig.h"
#include "mdl/GameInfo.h"
#include "mdl/GroupNode.h"
#include "mdl/LayerNode.h"
#include "mdl/Map.h"
#include "mdl/PatchNode.h"
#include "mdl/Tag.h"
#include "mdl/TagAttribute.h"
#include "mdl/WorldNode.h"

#include "kd/overload.h"

#include "vm/constants.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <string_view>
#include <unordered_map>

namespace tb::render
{
namespace
{

std::string toLower(std::string_view str)
{
  auto result = std::string{str};
  std::transform(result.begin(), result.end(), result.begin(), [](const char c) {
    return char(std::tolower(static_cast<unsigned char>(c)));
  });
  return result;
}

/**
 * The name a texture is known by inside the map, with any directory prefix removed. A
 * Quake face names its texture directly, but a Quake 3 shader is named by its path, and
 * light entities refer to the last component either way.
 */
std::string materialBaseName(const std::string& name)
{
  const auto slash = name.find_last_of("/\\");
  return toLower(slash == std::string::npos ? name : name.substr(slash + 1));
}

/**
 * Whether a face is sky.
 *
 * There is no portable answer, so all three conventions are tried: the Quake 2 style
 * surface flag named "sky" in the game config, the Quake 3 shader surface parameter, and
 * the plain Quake and GoldSrc convention of naming the texture "sky" something. A ray
 * that hits sky leaves the map rather than being shaded, so a false positive here would
 * punch a hole in the preview; all three tests are deliberately narrow.
 *
 * NOTE: sky faces are treated as a plain hole into the sky dome. Games lay out sky
 * textures in their own way, and none of that is reproduced here.
 */
struct SkyClassifier
{
  int skySurfaceFlag = 0;

  explicit SkyClassifier(const mdl::GameConfig& gameConfig)
    : skySurfaceFlag{gameConfig.faceAttribsConfig.surfaceFlags.flagValue("sky")}
  {
  }

  bool isSky(const gl::Material* material, const int surfaceFlags) const
  {
    if (skySurfaceFlag != 0 && (surfaceFlags & skySurfaceFlag) != 0)
    {
      return true;
    }

    if (!material)
    {
      return false;
    }

    if (material->surfaceParms().contains("sky"))
    {
      return true;
    }

    return materialBaseName(material->name()).starts_with("sky");
  }
};

/**
 * Reads back one material's texture so that the tracer can look up albedo without the GL
 * context.
 *
 * The image is boxed down to at most maxSize on a side. The preview is far too noisy for
 * texture detail to survive, and a map's worth of full resolution images is a lot of
 * memory to keep resident for no visible gain.
 */
PreviewMaterial readMaterial(
  const gl::Material& material, gl::Gl& gl, const size_t maxSize)
{
  auto result = PreviewMaterial{};
  result.name = material.name();

  const auto* texture = gl::getTexture(&material);
  if (!texture)
  {
    return result;
  }

  result.averageColor = vm::vec3f{texture->averageColor().to<RgbF>().toVec()};

  const auto sourceWidth = texture->width();
  const auto sourceHeight = texture->height();
  if (sourceWidth == 0 || sourceHeight == 0)
  {
    return result;
  }

  auto pixels = std::vector<unsigned char>{};
  if (texture->activate(gl, GL_NEAREST, GL_NEAREST))
  {
    // Reading as RGBA bytes makes every row a multiple of four bytes long, which is what
    // the default pack alignment expects, and lets the driver convert whatever internal
    // format the texture has, compressed ones included.
    pixels.resize(sourceWidth * sourceHeight * 4);
    gl.getTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
    texture->deactivate(gl);
  }
  else
  {
    // The texture has not been uploaded yet, which happens for materials on faces the
    // editor has not drawn. The loaded buffers are still around in that case.
    const auto& buffers = texture->buffersIfLoaded();
    if (buffers.empty())
    {
      return result;
    }

    const auto format = texture->format();
    const auto components = format == GL_RGBA  ? size_t(4)
                            : format == GL_RGB ? size_t(3)
                                               : size_t(0);
    if (
      components == 0 || buffers.front().size() < sourceWidth * sourceHeight * components)
    {
      return result;
    }

    const auto* source = buffers.front().data();
    pixels.resize(sourceWidth * sourceHeight * 4);
    for (size_t i = 0; i < sourceWidth * sourceHeight; ++i)
    {
      pixels[i * 4 + 0] = source[i * components + 0];
      pixels[i * 4 + 1] = source[i * components + 1];
      pixels[i * 4 + 2] = source[i * components + 2];
      pixels[i * 4 + 3] = components == 4 ? source[i * components + 3] : 255;
    }
  }

  if (pixels.empty())
  {
    return result;
  }

  const auto targetWidth = std::max(size_t(1), std::min(sourceWidth, maxSize));
  const auto targetHeight = std::max(size_t(1), std::min(sourceHeight, maxSize));

  result.width = targetWidth;
  result.height = targetHeight;
  result.texels.resize(targetWidth * targetHeight);

  for (size_t y = 0; y < targetHeight; ++y)
  {
    const auto sourceY0 = y * sourceHeight / targetHeight;
    const auto sourceY1 = std::max(sourceY0 + 1, (y + 1) * sourceHeight / targetHeight);

    for (size_t x = 0; x < targetWidth; ++x)
    {
      const auto sourceX0 = x * sourceWidth / targetWidth;
      const auto sourceX1 = std::max(sourceX0 + 1, (x + 1) * sourceWidth / targetWidth);

      auto sum = vm::vec3f{0, 0, 0};
      auto count = 0.0f;
      for (auto sy = sourceY0; sy < sourceY1; ++sy)
      {
        for (auto sx = sourceX0; sx < sourceX1; ++sx)
        {
          const auto* texel = &pixels[(sy * sourceWidth + sx) * 4];
          sum = sum
                + vm::vec3f{
                  float(texel[0]) / 255.0f,
                  float(texel[1]) / 255.0f,
                  float(texel[2]) / 255.0f};
          count += 1.0f;
        }
      }

      result.texels[y * targetWidth + x] =
        count > 0.0f ? sum / count : result.averageColor;
    }
  }

  return result;
}

/**
 * Binds the material cache to the GL context and resolution the current build is using,
 * so that collecting geometry only has to ask for an index.
 */
struct MaterialLookup
{
  PreviewMaterialCache& cache;
  gl::Gl& gl;
  size_t maxTextureSize;

  uint32_t indexOf(const gl::Material* material) const
  {
    return cache.indexOf(material, gl, maxTextureSize);
  }
};

/**
 * The lighting keys a brush model can carry, which apply to every face of that model.
 */
struct BrushModelLighting
{
  vm::vec3f minLight = vm::vec3f{0, 0, 0};
  int32_t objectChannelMask = 1;
  bool castsShadows = true;
  bool receivesLight = true;
};

std::vector<float> parsePropertyFloats(const std::string& str)
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

BrushModelLighting readBrushModelLighting(const mdl::EntityNodeBase* entityNode)
{
  auto result = BrushModelLighting{};
  if (!entityNode)
  {
    return result;
  }

  const auto& entity = entityNode->entity();
  const auto number = [&](const std::string& key) -> std::optional<float> {
    if (const auto* value = entity.property(key))
    {
      const auto numbers = parsePropertyFloats(*value);
      if (!numbers.empty())
      {
        return numbers.front();
      }
    }
    return std::nullopt;
  };

  if (const auto minLight = number("_minlight"))
  {
    auto color = vm::vec3f{1, 1, 1};
    for (const auto* key : {"_minlight_color", "_mincolor"})
    {
      if (const auto* value = entity.property(std::string{key}))
      {
        const auto numbers = parsePropertyFloats(*value);
        if (numbers.size() >= 3)
        {
          const auto raw = vm::vec3f{numbers[0], numbers[1], numbers[2]};
          const auto maximum = std::max({raw.x(), raw.y(), raw.z()});
          color = maximum > 1.0f ? raw / 255.0f : raw;
          break;
        }
      }
    }
    result.minLight = color * *minLight;
  }

  if (const auto mask = number("_object_channel_mask"))
  {
    result.objectChannelMask = int32_t(*mask);
  }

  // "_shadow" "-1" makes a brush model invisible to shadow rays, which mappers use to
  // keep a decorative model from darkening the room it sits in.
  if (const auto shadow = number("_shadow"))
  {
    result.castsShadows = *shadow >= 0.0f;
  }

  if (const auto lightIgnore = number("_lightignore"))
  {
    result.receivesLight = *lightIgnore == 0.0f;
  }

  return result;
}

struct TriangleSink
{
  PreviewScene& scene;
  vm::bbox3f::builder bounds;

  void add(
    const vm::vec3f& p0,
    const vm::vec3f& p1,
    const vm::vec3f& p2,
    const vm::vec2f& uv0,
    const vm::vec2f& uv1,
    const vm::vec2f& uv2,
    const vm::vec3f& normal,
    const PreviewTriangleShading& prototype)
  {
    const auto e1 = p1 - p0;
    const auto e2 = p2 - p0;

    // Degenerate triangles have no surface to shade and would divide by zero in the
    // intersection test, so they never make it into the hierarchy.
    const auto cross = vm::cross(e1, e2);
    if (vm::squared_length(cross) < 1.0e-12f)
    {
      return;
    }

    auto shading = prototype;
    shading.normal = normal;
    shading.uv0 = uv0;
    shading.uv1 = uv1;
    shading.uv2 = uv2;

    scene.trianglePositions.push_back(PreviewTrianglePos{p0, e1, e2});
    scene.triangleShading.push_back(shading);

    bounds.add(p0);
    bounds.add(p1);
    bounds.add(p2);
  }
};

void addBrushFace(
  const mdl::BrushNode& brushNode,
  const mdl::BrushFace& face,
  const BrushModelLighting& brushModel,
  const SkyClassifier& skyClassifier,
  const int lightSurfaceFlag,
  MaterialLookup& materials,
  TriangleSink& sink)
{
  const auto* material = face.material();
  const auto surfaceFlags = face.resolvedSurfaceFlags();

  auto shading = PreviewTriangleShading{};
  shading.materialIndex = materials.indexOf(material);
  shading.objectChannelMask = brushModel.objectChannelMask;
  shading.receivesLight = brushModel.receivesLight;
  shading.surfaceMinLight = brushModel.minLight;

  if (skyClassifier.isSky(material, surfaceFlags))
  {
    shading.kind = PreviewSurfaceKind::Sky;
    shading.occludes = false;
  }
  else
  {
    // Water, slime, lava, triggers, clip and hint brushes are all outside the solid hull
    // the compiler traces against, so light passes straight through them. TrenchBroom
    // already knows which those are: the game config tags them transparent.
    const auto nonSolid = brushNode.hasAttribute(mdl::TagAttributes::Transparency)
                          || face.hasAttribute(mdl::TagAttributes::Transparency);
    shading.kind = nonSolid ? PreviewSurfaceKind::NonSolid : PreviewSurfaceKind::Solid;
    shading.occludes = !nonSolid && brushModel.castsShadows;
  }

  // Quake 2 marks emissive faces with a surface flag and puts the brightness in the
  // face's value field, rather than using a light entity template.
  if (
    lightSurfaceFlag != 0 && (surfaceFlags & lightSurfaceFlag) != 0
    && shading.kind != PreviewSurfaceKind::Sky)
  {
    const auto value = face.resolvedSurfaceValue();
    if (value > 0.0f)
    {
      const auto color = sink.scene.materials[shading.materialIndex]->averageColor;
      shading.emission = color * value * PreviewLightUnitScale;
    }
  }

  const auto normal = vm::vec3f{face.normal()};
  const auto vertices = face.vertexPositions();
  if (vertices.size() < 3)
  {
    return;
  }

  const auto uv = [&](const vm::vec3d& position) { return face.uvCoords(position); };

  // A brush face is convex, so a fan from its first vertex covers it exactly.
  for (size_t i = 1; i + 1 < vertices.size(); ++i)
  {
    sink.add(
      vm::vec3f{vertices[0]},
      vm::vec3f{vertices[i]},
      vm::vec3f{vertices[i + 1]},
      uv(vertices[0]),
      uv(vertices[i]),
      uv(vertices[i + 1]),
      normal,
      shading);
  }
}

void addPatch(
  const mdl::PatchNode& patchNode,
  const BrushModelLighting& brushModel,
  const SkyClassifier& skyClassifier,
  MaterialLookup& materials,
  TriangleSink& sink)
{
  const auto& grid = patchNode.grid();
  if (grid.pointRowCount < 2 || grid.pointColumnCount < 2)
  {
    return;
  }

  const auto* material = patchNode.patch().material();

  auto shading = PreviewTriangleShading{};
  shading.materialIndex = materials.indexOf(material);
  shading.objectChannelMask = brushModel.objectChannelMask;
  shading.receivesLight = brushModel.receivesLight;
  shading.surfaceMinLight = brushModel.minLight;

  if (skyClassifier.isSky(material, 0))
  {
    shading.kind = PreviewSurfaceKind::Sky;
    shading.occludes = false;
  }
  else
  {
    shading.kind = PreviewSurfaceKind::Solid;
    shading.occludes = brushModel.castsShadows;
  }

  const auto addQuadTriangle = [&](
                                 const mdl::PatchGrid::Point& a,
                                 const mdl::PatchGrid::Point& b,
                                 const mdl::PatchGrid::Point& c) {
    const auto p0 = vm::vec3f{a.position};
    const auto p1 = vm::vec3f{b.position};
    const auto p2 = vm::vec3f{c.position};

    // Take the normal from the grid rather than from the winding, so that a curved patch
    // keeps the smooth normals it is drawn with.
    auto normal = vm::vec3f{a.normal + b.normal + c.normal};
    if (vm::squared_length(normal) < vm::constants<float>::almost_zero())
    {
      normal = vm::cross(p1 - p0, p2 - p0);
      if (vm::squared_length(normal) < vm::constants<float>::almost_zero())
      {
        return;
      }
    }

    sink.add(
      p0,
      p1,
      p2,
      vm::vec2f{a.uvCoords},
      vm::vec2f{b.uvCoords},
      vm::vec2f{c.uvCoords},
      vm::normalize(normal),
      shading);
  };

  for (size_t row = 0; row < grid.quadRowCount(); ++row)
  {
    for (size_t col = 0; col < grid.quadColumnCount(); ++col)
    {
      const auto& p00 = grid.point(row, col);
      const auto& p01 = grid.point(row, col + 1);
      const auto& p11 = grid.point(row + 1, col + 1);
      const auto& p10 = grid.point(row + 1, col);

      addQuadTriangle(p00, p01, p11);
      addQuadTriangle(p11, p10, p00);
    }
  }
}

/**
 * Turns the faces named by a surface light template into emitters.
 *
 * NOTE: ericw-tools subdivides an emitting face into a grid of point lights, and the
 * spacing is configurable. Here the triangles emit as area lights instead, which needs no
 * spacing at all and converges to the same answer, so "_surflightsubdivision" and the
 * "-surflight_subdivide" switch have no effect on the preview.
 */
void applySurfaceLights(PreviewScene& scene, const PreviewLighting& lighting)
{
  if (lighting.surfaceLights.empty())
  {
    return;
  }

  auto byName = std::unordered_map<std::string, const PreviewSurfaceLightTemplate*>{};
  for (const auto& surfaceLight : lighting.surfaceLights)
  {
    byName.emplace(materialBaseName(surfaceLight.materialName), &surfaceLight);
  }

  for (auto& shading : scene.triangleShading)
  {
    if (shading.kind == PreviewSurfaceKind::Sky)
    {
      continue;
    }

    const auto& materialName = scene.materials[shading.materialIndex]->name;
    const auto it = byName.find(materialBaseName(materialName));
    if (it == byName.end())
    {
      continue;
    }

    const auto& surfaceLight = *it->second;
    const auto scale = lighting.globals.surfaceLightScale * surfaceLight.styleScale;

    shading.emission =
      shading.emission
      + surfaceLight.color * surfaceLight.intensity * scale * PreviewLightUnitScale;
  }
}

/**
 * Collects the emissive triangles into a list that can be sampled by area.
 */
void buildEmitterList(PreviewScene& scene)
{
  scene.emitters.clear();
  scene.totalEmitterArea = 0.0f;

  for (uint32_t i = 0; i < uint32_t(scene.triangleShading.size()); ++i)
  {
    const auto& shading = scene.triangleShading[i];
    if (vm::squared_length(shading.emission) <= 0.0f)
    {
      continue;
    }

    const auto& position = scene.trianglePositions[i];
    const auto area = 0.5f * vm::length(vm::cross(position.e1, position.e2));
    if (area <= 0.0f)
    {
      continue;
    }

    scene.totalEmitterArea += area;
    scene.emitters.push_back(PreviewEmitter{i, area, scene.totalEmitterArea});
  }
}

void resolveProjectedTextures(PreviewScene& scene, const PreviewMaterialCache& materials)
{
  for (auto& light : scene.lights)
  {
    if (light.projectedTextureName.empty())
    {
      continue;
    }

    if (const auto index = materials.findByName(light.projectedTextureName))
    {
      light.projectedTextureIndex = int32_t(*index);
    }
  }
}

} // namespace

PreviewMaterialCache::PreviewMaterialCache()
{
  // Index 0 is the fallback for faces whose material could not be resolved.
  auto fallback = PreviewMaterial{};
  fallback.averageColor = vm::vec3f{0.5f, 0.5f, 0.5f};
  m_materials.push_back(std::make_shared<const PreviewMaterial>(std::move(fallback)));
}

uint32_t PreviewMaterialCache::indexOf(
  const gl::Material* material, gl::Gl& gl, const size_t maxTextureSize)
{
  if (!material)
  {
    return 0;
  }

  if (const auto it = m_indices.find(material); it != m_indices.end())
  {
    return it->second;
  }

  const auto index = uint32_t(m_materials.size());
  m_materials.push_back(
    std::make_shared<const PreviewMaterial>(readMaterial(*material, gl, maxTextureSize)));
  m_indices.emplace(material, index);
  m_indicesByName.emplace(materialBaseName(material->name()), index);
  return index;
}

std::optional<uint32_t> PreviewMaterialCache::findByName(const std::string& name) const
{
  const auto it = m_indicesByName.find(materialBaseName(name));
  return it != m_indicesByName.end() ? std::optional{it->second} : std::nullopt;
}

const std::vector<std::shared_ptr<const PreviewMaterial>>& PreviewMaterialCache::
  materials() const
{
  return m_materials;
}

void PreviewMaterialCache::clear()
{
  m_materials.clear();
  m_indices.clear();
  m_indicesByName.clear();

  auto fallback = PreviewMaterial{};
  fallback.averageColor = vm::vec3f{0.5f, 0.5f, 0.5f};
  m_materials.push_back(std::make_shared<const PreviewMaterial>(std::move(fallback)));
}

vm::vec3f PreviewMaterial::sample(const vm::vec2f& uv) const
{
  if (texels.empty() || width == 0 || height == 0)
  {
    return averageColor;
  }

  // Textures tile, so the coordinates wrap; std::fmod keeps the sign of its argument,
  // hence the extra shift for faces with negative offsets.
  const auto wrap = [](const float value, const size_t size) {
    const auto scaled = value * float(size);
    auto index = int64_t(std::floor(scaled)) % int64_t(size);
    if (index < 0)
    {
      index += int64_t(size);
    }
    return size_t(index);
  };

  const auto x = wrap(uv.x(), width);
  const auto y = wrap(uv.y(), height);
  return texels[y * width + x];
}

PreviewScene buildPreviewScene(
  const mdl::Map& map,
  gl::Gl& gl,
  PreviewMaterialCache& materialCache,
  const size_t maxTextureSize)
{
  auto scene = PreviewScene{};

  const auto lighting = extractLighting(map);
  scene.globals = lighting.globals;
  scene.lights = lighting.lights;

  const auto& gameConfig = map.gameInfo().gameConfig;
  const auto skyClassifier = SkyClassifier{gameConfig};
  const auto lightSurfaceFlag =
    gameConfig.faceAttribsConfig.surfaceFlags.flagValue("light");

  auto materials = MaterialLookup{materialCache, gl, maxTextureSize};
  auto sink = TriangleSink{scene, {}};

  const auto& editorContext = map.editorContext();

  map.worldNode().accept(kdl::overload(
    [](auto&& thisLambda, const mdl::WorldNode& node) { node.visitChildren(thisLambda); },
    [&](auto&& thisLambda, const mdl::LayerNode& node) {
      if (editorContext.visible(node))
      {
        node.visitChildren(thisLambda);
      }
    },
    [&](auto&& thisLambda, const mdl::GroupNode& node) {
      if (editorContext.visible(node))
      {
        node.visitChildren(thisLambda);
      }
    },
    [&](auto&& thisLambda, const mdl::EntityNode& node) {
      if (editorContext.visible(node))
      {
        node.visitChildren(thisLambda);
      }
    },
    [&](const mdl::BrushNode& node) {
      if (!editorContext.visible(node))
      {
        return;
      }

      const auto brushModel = readBrushModelLighting(node.entity());
      for (const auto& face : node.brush().faces())
      {
        if (editorContext.visible(node, face))
        {
          addBrushFace(
            node, face, brushModel, skyClassifier, lightSurfaceFlag, materials, sink);
        }
      }
    },
    [&](const mdl::PatchNode& node) {
      if (editorContext.visible(node))
      {
        addPatch(
          node, readBrushModelLighting(node.entity()), skyClassifier, materials, sink);
      }
    }));

  scene.bounds =
    sink.bounds.initialized() ? sink.bounds.bounds() : vm::bbox3f{0.0f, 0.0f};

  scene.hasSkyFaces = std::any_of(
    scene.triangleShading.begin(), scene.triangleShading.end(), [](const auto& shading) {
      return shading.kind == PreviewSurfaceKind::Sky;
    });

  // Every material the build touched is in the cache by now, so the scene can take the
  // whole list; the entries are shared rather than copied.
  scene.materials = materialCache.materials();

  applySurfaceLights(scene, lighting);
  buildEmitterList(scene);
  resolveProjectedTextures(scene, materialCache);

  return scene;
}

void buildPreviewSceneBvh(PreviewScene& scene)
{
  scene.bvh.build(scene.trianglePositions);
}

} // namespace tb::render
