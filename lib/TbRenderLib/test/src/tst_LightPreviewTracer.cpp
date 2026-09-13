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
#include "render/LightPreviewTracer.h"

#include "vm/constants.h"

#include <cmath>
#include <memory>
#include <optional>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

namespace tb::render
{
namespace
{

/**
 * A scene under construction, with a white floor to shade and a camera looking straight
 * down at it.
 *
 * Everything is arranged so that the expected answer can be worked out by hand: the
 * camera's field of view is a fraction of a degree wide, so every sample lands on the
 * same spot on the floor and the average over the samples converges to the value the
 * shading model should give for that one point.
 */
class TestScene
{
public:
  PreviewScene scene;
  uint32_t white = 0;

  TestScene()
  {
    // The expected values below are worked out in lightmap units, so the scaling a map
    // would get is left out; the section that covers it puts it back.
    scene.globals.rangeScale = 1.0f;

    scene.materials.push_back(std::make_shared<const PreviewMaterial>());

    auto material = PreviewMaterial{};
    material.name = "white";
    material.averageColor = vm::vec3f{1, 1, 1};
    scene.materials.push_back(
      std::make_shared<const PreviewMaterial>(std::move(material)));
    white = uint32_t(scene.materials.size() - 1);

    addQuad(
      vm::vec3f{-2000, -2000, 0},
      vm::vec3f{4000, 0, 0},
      vm::vec3f{0, 4000, 0},
      vm::vec3f{0, 0, 1});
  }

  /** Adds a material with the given colour and returns its index. */
  uint32_t addMaterial(const vm::vec3f& color)
  {
    auto material = PreviewMaterial{};
    material.averageColor = color;
    scene.materials.push_back(
      std::make_shared<const PreviewMaterial>(std::move(material)));
    return uint32_t(scene.materials.size() - 1);
  }

  void addQuad(
    const vm::vec3f& origin,
    const vm::vec3f& edgeU,
    const vm::vec3f& edgeV,
    const vm::vec3f& normal,
    const PreviewSurfaceKind kind = PreviewSurfaceKind::Solid,
    const vm::vec3f& emission = vm::vec3f{0, 0, 0},
    const float alpha = 1.0f,
    const std::optional<uint32_t> materialIndex = std::nullopt)
  {
    const auto addTriangle =
      [&](const vm::vec3f& p0, const vm::vec3f& p1, const vm::vec3f& p2) {
        auto shading = PreviewTriangleShading{};
        shading.normal = normal;
        shading.materialIndex = materialIndex.value_or(white);
        shading.kind = kind;
        shading.occludes = kind == PreviewSurfaceKind::Solid;
        shading.emission = emission;
        shading.alpha = alpha;

        scene.trianglePositions.push_back(PreviewTrianglePos{p0, p1 - p0, p2 - p0});
        scene.triangleShading.push_back(shading);
      };

    addTriangle(origin, origin + edgeU, origin + edgeU + edgeV);
    addTriangle(origin, origin + edgeU + edgeV, origin + edgeV);
  }

  /**
   * Adds a material whose texture is two texels wide: a hole on the left half and
   * picture on the right. Returns its index.
   */
  uint32_t addMaskedMaterial(const vm::vec3f& color)
  {
    auto material = PreviewMaterial{};
    material.name = "{masked";
    material.averageColor = color;
    material.width = 2;
    material.height = 1;
    material.texels = {color, color};
    material.masked = true;
    material.opaque = {uint8_t(0), uint8_t(1)};
    scene.materials.push_back(
      std::make_shared<const PreviewMaterial>(std::move(material)));
    return uint32_t(scene.materials.size() - 1);
  }

  /**
   * Adds a quad that lands on the same texel of its texture wherever it is hit, since
   * every corner of both its triangles takes the same coordinates.
   *
   * A ray crossing it therefore either meets a hole or meets picture, whichever the
   * coordinates name, and the test does not depend on where across the quad it crossed.
   */
  void addMaskedQuad(
    const vm::vec3f& origin,
    const vm::vec3f& edgeU,
    const vm::vec3f& edgeV,
    const vm::vec3f& normal,
    const uint32_t materialIndex,
    const vm::vec2f& uv,
    const vm::vec3f& emission = vm::vec3f{0, 0, 0})
  {
    addQuad(
      origin,
      edgeU,
      edgeV,
      normal,
      PreviewSurfaceKind::Solid,
      emission,
      1.0f,
      materialIndex);

    for (auto i = scene.triangleShading.size() - 2; i < scene.triangleShading.size(); ++i)
    {
      auto& shading = scene.triangleShading[i];
      shading.uv0 = shading.uv1 = shading.uv2 = uv;
      shading.maskedTexture = scene.materials[materialIndex]->masked;
    }
  }

  void addPointLight(const float intensity, const PreviewAttenuation attenuation)
  {
    scene.lights.push_back(makePointLight(intensity, attenuation));
  }

  static PreviewLight makePointLight(
    const float intensity, const PreviewAttenuation attenuation)
  {
    auto light = PreviewLight{};
    light.kind = PreviewLightKind::Point;
    light.origin = vm::vec3f{0, 0, 100};
    light.color = vm::vec3f{1, 1, 1};
    light.intensity = intensity;
    light.attenuation = attenuation;
    light.wait = 1.0f;
    light.angleScale = 0.0f;
    return light;
  }

  /** Collects the emissive triangles and builds the hierarchy. */
  void finish()
  {
    scene.emitters.clear();
    scene.totalEmitterArea = 0.0f;

    for (uint32_t i = 0; i < uint32_t(scene.triangleShading.size()); ++i)
    {
      if (vm::squared_length(scene.triangleShading[i].emission) <= 0.0f)
      {
        continue;
      }

      const auto& position = scene.trianglePositions[i];
      const auto area = 0.5f * vm::length(vm::cross(position.e1, position.e2));
      scene.totalEmitterArea += area;
      scene.emitters.push_back(PreviewEmitter{i, area, scene.totalEmitterArea});
    }

    buildPreviewSceneBvh(scene);
  }

  /**
   * Averages the given number of paths through the middle of the view.
   *
   * The camera height matters for the tests with something between the floor and the
   * light: from above the obstacle the camera would see the obstacle rather than the
   * floor that is supposed to be in shadow.
   */
  float shade(const int samples, const float cameraHeight = 200.0f) const
  {
    auto camera = PreviewCamera{};
    camera.position = vm::vec3f{0, 0, cameraHeight};
    camera.forward = vm::vec3f{0, 0, -1};
    camera.right = vm::vec3f{1, 0, 0};
    camera.up = vm::vec3f{0, 1, 0};
    camera.halfWidth = 0.0002f;
    camera.halfHeight = 0.0002f;
    camera.width = 1;
    camera.height = 1;

    auto settings = PreviewTraceSettings{};
    settings.maxBounces = m_bounces;
    settings.indirectLight = PreviewIndirectLight::On;

    auto sum = vm::vec3f{0, 0, 0};
    for (auto i = 0; i < samples; ++i)
    {
      sum = sum + tracePreviewPixel(scene, camera, settings, 0, 0, uint32_t(i));
    }
    return (sum / float(samples)).x();
  }

  void setBounces(const int bounces) { m_bounces = bounces; }

private:
  int m_bounces = 0;
};

/** A lightmap value of 255 is a fully lit white surface. */
float display(const float lightValue)
{
  return lightValue / 255.0f;
}

/** Coordinates landing on the hole of a material from addMaskedMaterial. */
const auto HoleUV = vm::vec2f{0.25f, 0.5f};

/** Coordinates landing on the picture of a material from addMaskedMaterial. */
const auto PictureUV = vm::vec2f{0.75f, 0.5f};

} // namespace

TEST_CASE("PreviewMaterial::transparentAt")
{
  auto material = PreviewMaterial{};
  material.width = 2;
  material.height = 1;
  material.texels = {vm::vec3f{1, 1, 1}, vm::vec3f{1, 1, 1}};
  material.opaque = {uint8_t(0), uint8_t(1)};

  SECTION("a texture with nothing see-through in it is asked nothing")
  {
    // The mask is only filled in for a masked material, so the flag has to be what
    // decides, not the mask.
    material.masked = false;
    CHECK_FALSE(material.transparentAt(HoleUV));
    CHECK_FALSE(material.transparentAt(PictureUV));
  }

  SECTION("the hole is where the mask says it is")
  {
    material.masked = true;
    CHECK(material.transparentAt(HoleUV));
    CHECK_FALSE(material.transparentAt(PictureUV));
  }

  SECTION("the mask tiles along with the texture")
  {
    material.masked = true;

    CHECK(material.transparentAt(HoleUV + vm::vec2f{3, 0}));
    CHECK_FALSE(material.transparentAt(PictureUV + vm::vec2f{3, 0}));

    CHECK(material.transparentAt(HoleUV - vm::vec2f{4, 2}));
    CHECK_FALSE(material.transparentAt(PictureUV - vm::vec2f{4, 2}));
  }

  SECTION("a material with no mask at all has no holes")
  {
    material.masked = true;
    material.opaque.clear();
    CHECK_FALSE(material.transparentAt(HoleUV));
  }
}

TEST_CASE("tracePreviewPixel")
{
  SECTION("attenuation")
  {
    SECTION("delay 3 does not fall off at all")
    {
      auto test = TestScene{};
      test.addPointLight(300.0f, PreviewAttenuation::None);
      test.finish();

      CHECK(test.shade(256) == Catch::Approx(display(300.0f)).margin(0.01));
    }

    SECTION("delay 0 falls off linearly")
    {
      auto test = TestScene{};
      test.addPointLight(300.0f, PreviewAttenuation::Linear);
      test.finish();

      CHECK(test.shade(256) == Catch::Approx(display(300.0f - 100.0f)).margin(0.01));
    }

    SECTION("delay 2 falls off with the square of the distance")
    {
      auto test = TestScene{};
      test.addPointLight(300.0f, PreviewAttenuation::InverseSquare);
      test.finish();

      const auto expected = 300.0f / ((100.0f * 100.0f) / (128.0f * 128.0f));
      CHECK(test.shade(256) == Catch::Approx(display(expected)).margin(0.02));
    }

    SECTION("_falloff overrides the formula and reaches zero where it says")
    {
      auto test = TestScene{};
      auto light = TestScene::makePointLight(300.0f, PreviewAttenuation::Linear);
      light.falloff = 200.0f;
      test.scene.lights.push_back(light);
      test.finish();

      CHECK(test.shade(256) == Catch::Approx(display(150.0f)).margin(0.01));
    }

    SECTION("_falloff is ignored on anything but a linear light")
    {
      auto test = TestScene{};
      auto light = TestScene::makePointLight(300.0f, PreviewAttenuation::None);
      light.falloff = 200.0f;
      test.scene.lights.push_back(light);
      test.finish();

      CHECK(test.shade(256) == Catch::Approx(display(300.0f)).margin(0.01));
    }

    SECTION("a negative light subtracts from what other lights put down")
    {
      auto test = TestScene{};
      test.addPointLight(300.0f, PreviewAttenuation::None);
      test.addPointLight(-100.0f, PreviewAttenuation::None);
      test.finish();

      CHECK(test.shade(256) == Catch::Approx(display(200.0f)).margin(0.01));
    }
  }

  SECTION("_anglescale mixes the cosine towards a flat response")
  {
    // The light sits at 45 degrees from the point being shaded.
    const auto cosTheta = 1.0f / std::sqrt(2.0f);

    const auto shadeAtAngleScale = [&](const float angleScale) {
      auto test = TestScene{};
      auto light = TestScene::makePointLight(300.0f, PreviewAttenuation::None);
      light.origin = vm::vec3f{100, 0, 100};
      light.angleScale = angleScale;
      test.scene.lights.push_back(light);
      test.finish();
      return test.shade(256);
    };

    SECTION("half way, which is the default")
    {
      CHECK(
        shadeAtAngleScale(0.5f)
        == Catch::Approx(display(300.0f * (0.5f + 0.5f * cosTheta))).margin(0.02));
    }

    SECTION("one uses the plain cosine")
    {
      CHECK(
        shadeAtAngleScale(1.0f)
        == Catch::Approx(display(300.0f * cosTheta)).margin(0.02));
    }

    SECTION("zero means the angle has no effect at all")
    {
      CHECK(shadeAtAngleScale(0.0f) == Catch::Approx(display(300.0f)).margin(0.02));
    }
  }

  SECTION("shadows")
  {
    SECTION("a solid surface blocks light")
    {
      auto test = TestScene{};
      test.addPointLight(300.0f, PreviewAttenuation::None);
      test.addQuad(
        vm::vec3f{-50, -50, 50},
        vm::vec3f{100, 0, 0},
        vm::vec3f{0, 100, 0},
        vm::vec3f{0, 0, -1});
      test.finish();

      CHECK(test.shade(256, 25.0f) == Catch::Approx(0.0).margin(0.001));
    }

    SECTION("light passes through a surface outside the solid hull")
    {
      auto test = TestScene{};
      test.addPointLight(300.0f, PreviewAttenuation::None);
      test.addQuad(
        vm::vec3f{-50, -50, 50},
        vm::vec3f{100, 0, 0},
        vm::vec3f{0, 100, 0},
        vm::vec3f{0, 0, -1},
        PreviewSurfaceKind::NonSolid);
      test.finish();

      CHECK(test.shade(256, 25.0f) == Catch::Approx(display(300.0f)).margin(0.01));
    }
  }

  SECTION("a liquid is seen through as well as lit through")
  {
    // A black surface over a white floor: whatever arrives is what came past the liquid,
    // so the reading is the share of the floor the liquid let through.
    const auto liquidAlpha = 0.4f;

    auto test = TestScene{};
    test.addPointLight(300.0f, PreviewAttenuation::None);
    const auto black = test.addMaterial(vm::vec3f{0, 0, 0});
    test.addQuad(
      vm::vec3f{-50, -50, 50},
      vm::vec3f{100, 0, 0},
      vm::vec3f{0, 100, 0},
      vm::vec3f{0, 0, 1},
      PreviewSurfaceKind::NonSolid,
      vm::vec3f{0, 0, 0},
      liquidAlpha,
      black);
    test.finish();

    CHECK(
      test.shade(256)
      == Catch::Approx(display(300.0f) * (1.0f - liquidAlpha)).margin(0.01));
  }

  SECTION("an opaque surface hides what is behind it")
  {
    auto test = TestScene{};
    test.addPointLight(300.0f, PreviewAttenuation::None);
    const auto black = test.addMaterial(vm::vec3f{0, 0, 0});
    test.addQuad(
      vm::vec3f{-50, -50, 50},
      vm::vec3f{100, 0, 0},
      vm::vec3f{0, 100, 0},
      vm::vec3f{0, 0, 1},
      PreviewSurfaceKind::NonSolid,
      vm::vec3f{0, 0, 0},
      1.0f,
      black);
    test.finish();

    CHECK(test.shade(64) == Catch::Approx(0.0).margin(0.001));
  }

  SECTION("a hole in a texture is not there for any ray")
  {
    // The quad the rays have to get past is black, so a ray that stops at it reads as
    // nothing at all, while the floor behind it is white and lit.
    const auto makeScene = [](const float height, const vm::vec2f& uv) {
      auto test = std::make_unique<TestScene>();
      test->addPointLight(300.0f, PreviewAttenuation::None);
      const auto masked = test->addMaskedMaterial(vm::vec3f{0, 0, 0});
      test->addMaskedQuad(
        vm::vec3f{-50, -50, height},
        vm::vec3f{100, 0, 0},
        vm::vec3f{0, 100, 0},
        vm::vec3f{0, 0, 1},
        masked,
        uv);
      test->finish();
      return test;
    };

    SECTION("the camera sees through a hole")
    {
      // Above the light, so that only the camera's own ray has to get past the quad.
      CHECK(
        makeScene(150.0f, HoleUV)->shade(64)
        == Catch::Approx(display(300.0f)).margin(0.01));
    }

    SECTION("the camera does not see through the picture around it")
    {
      CHECK(makeScene(150.0f, PictureUV)->shade(64) == Catch::Approx(0.0).margin(0.001));
    }

    SECTION("a shadow ray passes through a hole")
    {
      // Between the floor and the light, with the camera below it, so that only the
      // shadow ray has to get past the quad.
      CHECK(
        makeScene(50.0f, HoleUV)->shade(64, 25.0f)
        == Catch::Approx(display(300.0f)).margin(0.01));
    }

    SECTION("a shadow ray is stopped by the picture around it")
    {
      CHECK(
        makeScene(50.0f, PictureUV)->shade(64, 25.0f)
        == Catch::Approx(0.0).margin(0.001));
    }

    SECTION("light does not bounce off a hole")
    {
      // A white quad hanging over the floor with the light underneath it: its underside
      // is lit, so what it bounces down onto the floor adds to the floor's own reading,
      // but only where the quad is actually there.
      const auto shadeWithBounce = [](const std::optional<vm::vec2f> uv) {
        auto test = TestScene{};

        auto light = TestScene::makePointLight(100.0f, PreviewAttenuation::None);
        light.origin = vm::vec3f{0, 0, 20};
        test.scene.lights.push_back(light);

        if (uv)
        {
          const auto masked = test.addMaskedMaterial(vm::vec3f{1, 1, 1});
          test.addMaskedQuad(
            vm::vec3f{-400, -400, 50},
            vm::vec3f{800, 0, 0},
            vm::vec3f{0, 800, 0},
            vm::vec3f{0, 0, -1},
            masked,
            *uv);
        }

        test.finish();
        test.setBounces(1);
        return test.shade(4000, 25.0f);
      };

      const auto withoutQuad = shadeWithBounce(std::nullopt);
      const auto throughHoles = shadeWithBounce(HoleUV);
      const auto offThePicture = shadeWithBounce(PictureUV);

      // An all holes quad leaves the floor exactly as it was without one at all, and the
      // same quad made of picture is what a bounce off it is worth.
      CHECK(throughHoles == Catch::Approx(withoutQuad).margin(0.01));
      CHECK(offThePicture > withoutQuad + 0.05f);
    }

    SECTION("a hole in an emitting surface emits nothing")
    {
      const auto shadeEmitter = [](const vm::vec2f& uv) {
        auto test = TestScene{};
        const auto masked = test.addMaskedMaterial(vm::vec3f{0, 0, 0});
        test.addMaskedQuad(
          vm::vec3f{-200, -200, 100},
          vm::vec3f{400, 0, 0},
          vm::vec3f{0, 400, 0},
          vm::vec3f{0, 0, -1},
          masked,
          uv,
          vm::vec3f{0.5f, 0.5f, 0.5f});
        test.finish();
        return test.shade(4000, 50.0f);
      };

      CHECK(shadeEmitter(HoleUV) == Catch::Approx(0.0).margin(0.001));
      CHECK(shadeEmitter(PictureUV) > 0.1f);
    }
  }

  SECTION("suns")
  {
    const auto makeSun = [](const float intensity) {
      auto sun = PreviewLight{};
      sun.kind = PreviewLightKind::Sun;
      sun.direction = vm::vec3f{0, 0, -1};
      sun.intensity = intensity;
      sun.attenuation = PreviewAttenuation::None;
      sun.angleScale = 0.0f;
      return sun;
    };

    SECTION("a sun lights whatever its shadow ray can escape from")
    {
      auto test = TestScene{};
      test.scene.lights.push_back(makeSun(400.0f));
      test.finish();

      CHECK(test.shade(256) == Catch::Approx(display(400.0f)).margin(0.01));
    }

    SECTION("sky does not block a sun")
    {
      auto test = TestScene{};
      test.scene.lights.push_back(makeSun(400.0f));
      test.addQuad(
        vm::vec3f{-500, -500, 500},
        vm::vec3f{1000, 0, 0},
        vm::vec3f{0, 1000, 0},
        vm::vec3f{0, 0, -1},
        PreviewSurfaceKind::Sky);
      test.finish();

      CHECK(test.shade(256) == Catch::Approx(display(400.0f)).margin(0.01));
    }

    SECTION("a solid ceiling does block a sun")
    {
      auto test = TestScene{};
      test.scene.lights.push_back(makeSun(400.0f));
      test.addQuad(
        vm::vec3f{-500, -500, 500},
        vm::vec3f{1000, 0, 0},
        vm::vec3f{0, 1000, 0},
        vm::vec3f{0, 0, -1});
      test.finish();

      CHECK(test.shade(64, 25.0f) == Catch::Approx(0.0).margin(0.001));
    }
  }

  SECTION("an emitting surface lights what faces it")
  {
    // Checked against the form factor between a surface element and a coaxial parallel
    // rectangle, which is the closed form of what the estimator is sampling.
    auto test = TestScene{};
    const auto half = 200.0f;
    const auto height = 100.0f;
    test.addQuad(
      vm::vec3f{-half, -half, height},
      vm::vec3f{2.0f * half, 0, 0},
      vm::vec3f{0, 2.0f * half, 0},
      vm::vec3f{0, 0, -1},
      PreviewSurfaceKind::Solid,
      vm::vec3f{0.5f, 0.5f, 0.5f});
    test.finish();

    const auto ratio = half / height;
    const auto root = std::sqrt(1.0f + ratio * ratio);
    const auto formFactor =
      2.0f / vm::constants<float>::pi() * 2.0f * (ratio / root) * std::atan(ratio / root);

    CHECK(test.shade(30000, 50.0f) == Catch::Approx(0.5f * formFactor).margin(0.02));
  }

  SECTION("\"_surflight_atten\" fades a surface light faster")
  {
    const auto lit = [](const float atten) {
      auto test = TestScene{};
      test.addQuad(
        vm::vec3f{-200, -200, 100},
        vm::vec3f{400, 0, 0},
        vm::vec3f{0, 400, 0},
        vm::vec3f{0, 0, -1},
        PreviewSurfaceKind::Solid,
        vm::vec3f{0.5f, 0.5f, 0.5f});

      for (auto i = test.scene.triangleShading.size() - 2;
           i < test.scene.triangleShading.size();
           ++i)
      {
        test.scene.triangleShading[i].emissionAtten = atten;
      }
      test.finish();
      return test.shade(30000, 50.0f);
    };

    // Asking it to fade twice as fast reads as if it were twice as far away, so a
    // quarter of the light lands.
    const auto plain = lit(1.0f);
    CHECK(plain > 0.05f);
    CHECK(lit(2.0f) == Catch::Approx(plain / 4.0f).epsilon(0.05));
  }

  SECTION("a sky gives off light in every direction and can be pushed further away")
  {
    const auto lit = [](const bool omnidirectional, const float distanceOffset) {
      auto test = TestScene{};
      // Turned away from the floor, so only an omnidirectional emitter reaches it.
      test.addQuad(
        vm::vec3f{-200, -200, 100},
        vm::vec3f{400, 0, 0},
        vm::vec3f{0, 400, 0},
        vm::vec3f{0, 0, 1},
        PreviewSurfaceKind::Solid,
        vm::vec3f{0.5f, 0.5f, 0.5f});

      for (auto i = test.scene.triangleShading.size() - 2;
           i < test.scene.triangleShading.size();
           ++i)
      {
        test.scene.triangleShading[i].omnidirectionalEmitter = omnidirectional;
        test.scene.triangleShading[i].emissionDistanceOffset = distanceOffset;
      }
      test.finish();
      return test.shade(30000, 50.0f);
    };

    // Facing away, a surface light reaches nothing below it.
    CHECK(lit(false, 0.0f) == Catch::Approx(0.0).margin(0.001));

    // A sky reaches it whichever way the face it is drawn on points.
    const auto near = lit(true, 0.0f);
    CHECK(near > 0.05f);

    // Pushing it further away dims it.
    CHECK(lit(true, 200.0f) < near * 0.8f);
  }

  SECTION("indirect light follows the map unless the preview overrides it")
  {
    const auto shadeWith =
      [](const int32_t mapBounces, const PreviewIndirectLight indirect) {
        auto test = TestScene{};
        test.scene.globals.bounces = mapBounces;
        test.scene.globals.skyDome = vm::vec3f{100, 100, 100};
        test.addQuad(
          vm::vec3f{-20000, -20000, 500},
          vm::vec3f{40000, 0, 0},
          vm::vec3f{0, 40000, 0},
          vm::vec3f{0, 0, -1},
          PreviewSurfaceKind::Sky);
        test.finish();

        auto camera = PreviewCamera{};
        camera.position = vm::vec3f{0, 0, 200};
        camera.forward = vm::vec3f{0, 0, -1};
        camera.right = vm::vec3f{1, 0, 0};
        camera.up = vm::vec3f{0, 1, 0};
        camera.halfWidth = camera.halfHeight = 0.0002f;
        camera.width = camera.height = 1;

        auto settings = PreviewTraceSettings{};
        settings.maxBounces = 1;
        settings.indirectLight = indirect;

        auto sum = vm::vec3f{0, 0, 0};
        const auto samples = 4000;
        for (auto i = 0; i < samples; ++i)
        {
          sum = sum + tracePreviewPixel(test.scene, camera, settings, 0, 0, uint32_t(i));
        }
        return (sum / float(samples)).x();
      };

    // The dome only reaches a surface by way of a bounce, so it is a direct readout of
    // whether indirect light was computed.
    CHECK(
      shadeWith(0, PreviewIndirectLight::FromMap) == Catch::Approx(0.0).margin(0.001));
    CHECK(shadeWith(1, PreviewIndirectLight::FromMap) > 0.3f);
    CHECK(shadeWith(0, PreviewIndirectLight::On) > 0.3f);
    CHECK(shadeWith(1, PreviewIndirectLight::Off) == Catch::Approx(0.0).margin(0.001));
  }

  SECTION("a light's own \"_bouncescale\" scales only what it contributes to a bounce")
  {
    // A ceiling above the light bounces onto the floor the camera is looking at, so the
    // shade is direct light plus a bounce, and only the bounce carries "_bouncescale".
    const auto shadeWith = [](const float bounceScale) {
      auto test = TestScene{};
      test.addQuad(
        vm::vec3f{-400, -400, 0},
        vm::vec3f{800, 0, 0},
        vm::vec3f{0, 800, 0},
        vm::vec3f{0, 0, 1},
        PreviewSurfaceKind::Solid,
        vm::vec3f{0.6f, 0.6f, 0.6f});
      test.addQuad(
        vm::vec3f{-400, -400, 400},
        vm::vec3f{800, 0, 0},
        vm::vec3f{0, 800, 0},
        vm::vec3f{0, 0, -1},
        PreviewSurfaceKind::Solid,
        vm::vec3f{0.6f, 0.6f, 0.6f});

      auto light = TestScene::makePointLight(400.0f, PreviewAttenuation::Linear);
      light.bounceScale = bounceScale;
      test.scene.lights.push_back(light);

      test.setBounces(1);
      test.finish();
      return test.shade(6000);
    };

    const auto none = shadeWith(0.0f);
    const auto half = shadeWith(0.5f);
    const auto full = shadeWith(1.0f);

    // Turning the light's bounce off leaves the direct light it casts untouched.
    CHECK(none > 0.0f);
    CHECK(full > none);
    // What the bounce adds is proportional to the scale.
    CHECK(half - none == Catch::Approx(0.5f * (full - none)).epsilon(0.05));
  }

  SECTION("a light carrying a style bounces only when the map asks")
  {
    // The same room as above: direct light plus a bounce off the ceiling, where the
    // light now carries a style. "_bouncestyled" is what lets that bounce happen.
    const auto shadeWith = [](const int32_t style, const bool bounceStyled) {
      auto test = TestScene{};
      test.scene.globals.bounceStyled = bounceStyled;
      test.addQuad(
        vm::vec3f{-400, -400, 0},
        vm::vec3f{800, 0, 0},
        vm::vec3f{0, 800, 0},
        vm::vec3f{0, 0, 1},
        PreviewSurfaceKind::Solid,
        vm::vec3f{0.6f, 0.6f, 0.6f});
      test.addQuad(
        vm::vec3f{-400, -400, 400},
        vm::vec3f{800, 0, 0},
        vm::vec3f{0, 800, 0},
        vm::vec3f{0, 0, -1},
        PreviewSurfaceKind::Solid,
        vm::vec3f{0.6f, 0.6f, 0.6f});

      auto light = TestScene::makePointLight(400.0f, PreviewAttenuation::Linear);
      light.style = style;
      test.scene.lights.push_back(light);

      test.setBounces(1);
      test.finish();
      return test.shade(6000);
    };

    const auto steady = shadeWith(0, false);
    const auto styled = shadeWith(3, false);
    const auto styledAllowed = shadeWith(3, true);

    // A styled light lights the floor directly either way, but its bounce is held back.
    CHECK(styled < steady);
    // Letting it bounce brings the room back to what a steady light would have given.
    CHECK(styledAllowed == Catch::Approx(steady).epsilon(0.02));
  }

  SECTION("the sky dome lights whatever can see sky")
  {
    auto test = TestScene{};
    test.scene.globals.skyDome = vm::vec3f{100, 100, 100};
    test.addQuad(
      vm::vec3f{-20000, -20000, 500},
      vm::vec3f{40000, 0, 0},
      vm::vec3f{0, 40000, 0},
      vm::vec3f{0, 0, -1},
      PreviewSurfaceKind::Sky);
    test.setBounces(1);
    test.finish();

    CHECK(test.shade(20000) == Catch::Approx(display(100.0f)).margin(0.02));
  }

  SECTION("minimum light")
  {
    SECTION("lifts an unlit surface to its own value")
    {
      auto test = TestScene{};
      test.scene.globals.minLight = vm::vec3f{50, 50, 50};
      test.finish();

      CHECK(test.shade(64) == Catch::Approx(display(50.0f)).margin(0.001));
    }

    SECTION("does not add to a surface that is already brighter")
    {
      auto test = TestScene{};
      test.scene.globals.minLight = vm::vec3f{50, 50, 50};
      test.addPointLight(300.0f, PreviewAttenuation::None);
      test.finish();

      CHECK(test.shade(256) == Catch::Approx(display(300.0f)).margin(0.01));
    }

    SECTION("delay 4 lifts only what the entity can see")
    {
      auto lit = TestScene{};
      auto light = TestScene::makePointLight(120.0f, PreviewAttenuation::LocalMinLight);
      light.kind = PreviewLightKind::LocalMinLight;
      lit.scene.lights.push_back(light);
      lit.finish();

      CHECK(lit.shade(64) == Catch::Approx(display(120.0f)).margin(0.001));

      auto blocked = TestScene{};
      blocked.scene.lights.push_back(light);
      blocked.addQuad(
        vm::vec3f{-50, -50, 50},
        vm::vec3f{100, 0, 0},
        vm::vec3f{0, 100, 0},
        vm::vec3f{0, 0, -1});
      blocked.finish();

      CHECK(blocked.shade(64, 25.0f) == Catch::Approx(0.0).margin(0.001));
    }
  }

  SECTION("spotlights")
  {
    const auto makeSpot = [](const vm::vec3f& direction) {
      auto light = TestScene::makePointLight(300.0f, PreviewAttenuation::None);
      light.spot = true;
      light.direction = direction;
      light.cosOuterCone = std::cos(vm::to_radians(20.0f));
      light.cosInnerCone = light.cosOuterCone;
      return light;
    };

    SECTION("light inside the cone arrives")
    {
      auto test = TestScene{};
      test.scene.lights.push_back(makeSpot(vm::vec3f{0, 0, -1}));
      test.finish();

      CHECK(test.shade(256) == Catch::Approx(display(300.0f)).margin(0.01));
    }

    SECTION("light outside the cone does not")
    {
      auto test = TestScene{};
      test.scene.lights.push_back(makeSpot(vm::vec3f{1, 0, 0}));
      test.finish();

      CHECK(test.shade(64) == Catch::Approx(0.0).margin(0.001));
    }
  }

  SECTION("the scaling a map asks for comes last")
  {
    // The compilers scale a finished lightmap rather than each light: minimum light and
    // surface lights are in it by then, the ceiling acts on the whole colour at twice
    // its own value, and the range scale is applied after both.
    const auto lit = [](const float range, const float maxLight, const float minLight) {
      auto test = TestScene{};
      test.scene.globals.rangeScale = range;
      test.scene.globals.maxLight = maxLight;
      test.scene.globals.minLight = vm::vec3f{minLight, minLight, minLight};
      test.addPointLight(300.0f, PreviewAttenuation::None);
      test.finish();
      return test.shade(64);
    };

    SECTION("\"_range\" halves what a light puts down")
    {
      CHECK(lit(0.5f, 0.0f, 0.0f) == Catch::Approx(display(150.0f)).margin(0.01));
    }

    SECTION("\"_maxlight\" is a ceiling at twice its value, below the range scale")
    {
      // 300 is above the ceiling of 2 * 100, so it comes down to 200 and is then halved.
      CHECK(lit(0.5f, 100.0f, 0.0f) == Catch::Approx(display(100.0f)).margin(0.01));

      // A light already under the ceiling is left alone.
      CHECK(lit(0.5f, 200.0f, 0.0f) == Catch::Approx(display(150.0f)).margin(0.01));
    }

    SECTION("minimum light is scaled along with everything else")
    {
      // The floor is 400, above what the light puts down, and is then halved.
      CHECK(lit(0.5f, 0.0f, 400.0f) == Catch::Approx(display(200.0f)).margin(0.01));
    }
  }

  SECTION("a ceiling brings the whole colour down together")
  {
    // Cutting each channel off on its own would turn a warm light grey as it clamped.
    auto test = TestScene{};
    test.scene.globals.rangeScale = 1.0f;
    test.scene.globals.maxLight = 100.0f;

    auto light = TestScene::makePointLight(400.0f, PreviewAttenuation::None);
    light.color = vm::vec3f{1.0f, 0.5f, 0.25f};
    test.scene.lights.push_back(light);
    test.finish();

    auto camera = PreviewCamera{};
    camera.position = vm::vec3f{0, 0, 200};
    camera.forward = vm::vec3f{0, 0, -1};
    camera.right = vm::vec3f{1, 0, 0};
    camera.up = vm::vec3f{0, 1, 0};
    camera.halfWidth = camera.halfHeight = 0.0002f;
    camera.width = camera.height = 1;

    auto settings = PreviewTraceSettings{};
    auto sum = vm::vec3f{0, 0, 0};
    for (auto i = 0; i < 64; ++i)
    {
      sum = sum + tracePreviewPixel(test.scene, camera, settings, 0, 0, uint32_t(i));
    }
    const auto color = sum / 64.0f;

    // The brightest channel sits on the ceiling of 2 * 100 and the others keep their
    // share of it.
    CHECK(color.x() == Catch::Approx(display(200.0f)).margin(0.01));
    CHECK(color.y() == Catch::Approx(display(100.0f)).margin(0.01));
    CHECK(color.z() == Catch::Approx(display(50.0f)).margin(0.01));
  }

  SECTION("\"_addmin\" adds the minimum light instead of putting a floor under it")
  {
    const auto lit = [](const bool addMinLight) {
      auto test = TestScene{};
      test.scene.globals.addMinLight = addMinLight;
      test.scene.globals.minLight = vm::vec3f{100, 100, 100};
      test.addPointLight(300.0f, PreviewAttenuation::None);
      test.finish();
      return test.shade(64);
    };

    // A floor under 300 does nothing; added to it, it lifts the surface to 400.
    CHECK(lit(false) == Catch::Approx(display(300.0f)).margin(0.01));
    CHECK(lit(true) == Catch::Approx(display(400.0f)).margin(0.01));
  }

  SECTION("\"_bleed\" lets a light round the corner onto the back of a surface")
  {
    const auto lit = [](const bool bleed) {
      auto test = TestScene{};

      // The light is under the floor, which faces up, so the only thing between the two
      // is the angle: the floor is asked not to cast a shadow of its own.
      for (auto& shading : test.scene.triangleShading)
      {
        shading.occludes = false;
      }

      auto light = TestScene::makePointLight(300.0f, PreviewAttenuation::None);
      light.origin = vm::vec3f{0, 0, -100};
      light.bleed = bleed;
      light.angleScale = 1.0f;
      test.scene.lights.push_back(light);
      test.finish();
      return test.shade(64);
    };

    CHECK(lit(false) == Catch::Approx(0.0).margin(0.001));
    CHECK(lit(true) == Catch::Approx(display(300.0f)).margin(0.01));
  }

  SECTION("a brush model's own ceiling is used instead of the map's")
  {
    auto test = TestScene{};
    test.scene.globals.maxLight = 200.0f;
    test.addPointLight(300.0f, PreviewAttenuation::None);
    test.finish();

    // 2 * 50 is below what the light puts down, so the floor comes down to it; the map's
    // own ceiling of 2 * 200 would have left it alone.
    for (auto& shading : test.scene.triangleShading)
    {
      shading.surfaceMaxLight = 50.0f;
    }

    CHECK(test.shade(64) == Catch::Approx(display(100.0f)).margin(0.01));
  }

  SECTION("\"_lightcolorscale\" takes the colour out of what a model receives")
  {
    auto test = TestScene{};
    auto light = TestScene::makePointLight(300.0f, PreviewAttenuation::None);
    light.color = vm::vec3f{1.0f, 0.0f, 0.0f};
    test.scene.lights.push_back(light);
    test.finish();

    for (auto& shading : test.scene.triangleShading)
    {
      shading.lightColorScale = 0.0f;
    }

    auto camera = PreviewCamera{};
    camera.position = vm::vec3f{0, 0, 200};
    camera.forward = vm::vec3f{0, 0, -1};
    camera.right = vm::vec3f{1, 0, 0};
    camera.up = vm::vec3f{0, 1, 0};
    camera.halfWidth = camera.halfHeight = 0.0002f;
    camera.width = camera.height = 1;

    auto settings = PreviewTraceSettings{};
    auto sum = vm::vec3f{0, 0, 0};
    for (auto i = 0; i < 64; ++i)
    {
      sum = sum + tracePreviewPixel(test.scene, camera, settings, 0, 0, uint32_t(i));
    }
    const auto color = sum / 64.0f;

    // A red light on a grey surface: every channel ends up at what the red one was.
    CHECK(color.x() == Catch::Approx(display(300.0f)).margin(0.01));
    CHECK(color.y() == Catch::Approx(color.x()).margin(0.01));
    CHECK(color.z() == Catch::Approx(color.x()).margin(0.01));
  }

  SECTION("a model can be told to cast a shadow on itself alone")
  {
    // The obstacle belongs to a model of its own, and is asked to shadow only that
    // model; the floor underneath is the world, so the light reaches it.
    const auto lit = [](const bool selfOnly) {
      auto test = TestScene{};
      test.addPointLight(300.0f, PreviewAttenuation::None);
      test.addQuad(
        vm::vec3f{-50, -50, 50},
        vm::vec3f{100, 0, 0},
        vm::vec3f{0, 100, 0},
        vm::vec3f{0, 0, 1});

      for (auto i = test.scene.triangleShading.size() - 2;
           i < test.scene.triangleShading.size();
           ++i)
      {
        test.scene.triangleShading[i].objectIndex = 1;
        test.scene.triangleShading[i].shadowsSelfOnly = selfOnly;
      }

      test.finish();
      return test.shade(64, 25.0f);
    };

    CHECK(lit(false) == Catch::Approx(0.0).margin(0.001));
    CHECK(lit(true) == Catch::Approx(display(300.0f)).margin(0.01));
  }

  SECTION("a model can be told to cast a shadow on the world alone")
  {
    // The floor belongs to a model rather than to the world, so an obstacle asked to
    // shadow the world alone is not in the way of it.
    const auto lit = [](const bool worldOnly) {
      auto test = TestScene{};
      test.addPointLight(300.0f, PreviewAttenuation::None);

      for (auto& shading : test.scene.triangleShading)
      {
        shading.objectIndex = 2;
      }

      test.addQuad(
        vm::vec3f{-50, -50, 50},
        vm::vec3f{100, 0, 0},
        vm::vec3f{0, 100, 0},
        vm::vec3f{0, 0, 1});

      for (auto i = test.scene.triangleShading.size() - 2;
           i < test.scene.triangleShading.size();
           ++i)
      {
        test.scene.triangleShading[i].objectIndex = 1;
        test.scene.triangleShading[i].shadowsWorldOnly = worldOnly;
      }

      test.finish();
      return test.shade(64, 25.0f);
    };

    CHECK(lit(false) == Catch::Approx(0.0).margin(0.001));
    CHECK(lit(true) == Catch::Approx(display(300.0f)).margin(0.01));
  }

  SECTION("dirt darkens a surface by how shut in it is")
  {
    // A floor with walls close in on either side is darkened; the same floor out in the
    // open is not.
    const auto lit = [](const bool dirt, const bool walls, const float scale = 1.0f) {
      auto test = TestScene{};
      test.scene.globals.dirt = dirt;
      test.scene.globals.dirtInUse = dirt;
      test.scene.globals.dirtScale = scale;
      test.scene.globals.dirtDepth = 128.0f;
      test.addPointLight(300.0f, PreviewAttenuation::None);

      if (walls)
      {
        // A narrow slot around the point the camera looks at, close enough to shut it in
        // but not between it and the light overhead.
        for (const auto x : {-16.0f, 16.0f})
        {
          test.addQuad(
            vm::vec3f{x, -64, 0},
            vm::vec3f{0, 128, 0},
            vm::vec3f{0, 0, 64},
            vm::vec3f{x < 0 ? 1.0f : -1.0f, 0, 0});
        }
      }

      test.finish();
      return test.shade(256, 40.0f);
    };

    const auto open = lit(true, false);
    const auto shutIn = lit(true, true);
    const auto shutInWithoutDirt = lit(false, true);

    // Out in the open there is nothing to darken it, so it reads the same either way.
    CHECK(open == Catch::Approx(display(300.0f)).margin(0.02));
    CHECK(shutInWithoutDirt == Catch::Approx(display(300.0f)).margin(0.02));

    // Shut in, dirt takes a bite out of it.
    CHECK(shutIn < open * 0.9f);
    CHECK(shutIn > 0.0f);

    // Half the scale takes half as much away.
    const auto halfScale = lit(true, true, 0.5f);
    CHECK(halfScale > shutIn);
    CHECK(halfScale < open);
  }

  SECTION("a surface can be kept out of the dirt")
  {
    auto test = TestScene{};
    test.scene.globals.dirt = true;
    test.scene.globals.dirtInUse = true;
    test.addPointLight(300.0f, PreviewAttenuation::None);
    for (const auto x : {-16.0f, 16.0f})
    {
      test.addQuad(
        vm::vec3f{x, -64, 0},
        vm::vec3f{0, 128, 0},
        vm::vec3f{0, 0, 64},
        vm::vec3f{x < 0 ? 1.0f : -1.0f, 0, 0});
    }
    for (auto& shading : test.scene.triangleShading)
    {
      shading.noDirt = true;
    }
    test.finish();

    CHECK(test.shade(256, 40.0f) == Catch::Approx(display(300.0f)).margin(0.02));
  }

  SECTION("a smoothed surface is shaded with the normal its corners carry")
  {
    const auto lit = [](const std::optional<vm::vec3f>& cornerNormal) {
      auto test = TestScene{};

      auto light = TestScene::makePointLight(300.0f, PreviewAttenuation::None);
      light.angleScale = 1.0f;
      test.scene.lights.push_back(light);

      if (cornerNormal)
      {
        test.scene.smoothNormals.push_back(
          PreviewSmoothNormals{*cornerNormal, *cornerNormal, *cornerNormal});
        for (auto& shading : test.scene.triangleShading)
        {
          shading.smoothIndex = 0;
        }
      }

      test.finish();
      return test.shade(64);
    };

    // Flat, the floor faces the light square on.
    CHECK(lit(std::nullopt) == Catch::Approx(display(300.0f)).margin(0.01));

    // Leaning sixty degrees away from it, half of the light lands.
    const auto tilted = vm::normalize(vm::vec3f{std::sqrt(3.0f) / 2.0f, 0, 0.5f});
    CHECK(lit(tilted) == Catch::Approx(display(150.0f)).margin(0.02));
  }

  SECTION("\"_minlight_mottle\" breaks the minimum light up")
  {
    const auto lit = [](const bool mottle, const float cameraX) {
      auto test = TestScene{};
      test.scene.globals.minLight = vm::vec3f{100, 100, 100};
      test.scene.globals.minLightColor = vm::vec3f{1, 1, 1};
      test.scene.globals.minLightMottle = mottle;
      test.finish();

      auto camera = PreviewCamera{};
      camera.position = vm::vec3f{cameraX, 0, 200};
      camera.forward = vm::vec3f{0, 0, -1};
      camera.right = vm::vec3f{1, 0, 0};
      camera.up = vm::vec3f{0, 1, 0};
      camera.halfWidth = camera.halfHeight = 0.0002f;
      camera.width = camera.height = 1;

      auto settings = PreviewTraceSettings{};
      auto sum = vm::vec3f{0, 0, 0};
      for (auto i = 0; i < 16; ++i)
      {
        sum = sum + tracePreviewPixel(test.scene, camera, settings, 0, 0, uint32_t(i));
      }
      return (sum / 16.0f).x();
    };

    const auto flat = lit(false, 0.0f);

    // Without it the floor reads the same everywhere.
    CHECK(lit(false, 144.0f) == Catch::Approx(flat).margin(0.001));

    // With it the readings wander from place to place. Two places can land on nearly the
    // same value, so what is checked is the spread across several of them.
    auto lowest = 1.0e9f;
    auto highest = -1.0e9f;
    for (const auto x : {0.0f, 144.0f, 240.0f, 72.0f, 192.0f})
    {
      const auto value = lit(true, x);
      lowest = std::min(lowest, value);
      highest = std::max(highest, value);

      // The noise only ever adds, and never more than forty eight lightmap units on top
      // of the hundred already there.
      CHECK(value >= flat - 0.001f);
      CHECK(value <= display(148.0f) + 0.001f);
    }

    CHECK(highest - lowest > display(20.0f));
  }

  SECTION("a surface light can light a room without looking lit itself")
  {
    // "_surflight_minlight_scale" parts what an emitting surface shows from what it
    // gives off.
    const auto seen = [](const float selfScale) {
      auto test = TestScene{};
      test.addQuad(
        vm::vec3f{-50, -50, 100},
        vm::vec3f{100, 0, 0},
        vm::vec3f{0, 100, 0},
        vm::vec3f{0, 0, -1},
        PreviewSurfaceKind::Solid,
        vm::vec3f{0.5f, 0.5f, 0.5f});

      for (auto i = test.scene.triangleShading.size() - 2;
           i < test.scene.triangleShading.size();
           ++i)
      {
        test.scene.triangleShading[i].selfEmissionScale = selfScale;
      }
      test.finish();

      // Looking up at the emitter from below it.
      auto camera = PreviewCamera{};
      camera.position = vm::vec3f{0, 0, 50};
      camera.forward = vm::vec3f{0, 0, 1};
      camera.right = vm::vec3f{1, 0, 0};
      camera.up = vm::vec3f{0, 1, 0};
      camera.halfWidth = camera.halfHeight = 0.0002f;
      camera.width = camera.height = 1;

      auto settings = PreviewTraceSettings{};
      auto sum = vm::vec3f{0, 0, 0};
      for (auto i = 0; i < 16; ++i)
      {
        sum = sum + tracePreviewPixel(test.scene, camera, settings, 0, 0, uint32_t(i));
      }
      return (sum / 16.0f).x();
    };

    CHECK(seen(1.0f) == Catch::Approx(0.5).margin(0.01));
    CHECK(seen(0.0f) == Catch::Approx(0.0).margin(0.001));
  }

  SECTION("a light does not reach a surface on another channel")
  {
    auto test = TestScene{};
    auto light = TestScene::makePointLight(300.0f, PreviewAttenuation::None);
    light.lightChannelMask = 2;
    light.shadowChannelMask = 2;
    test.scene.lights.push_back(light);
    test.finish();

    CHECK(test.shade(64) == Catch::Approx(0.0).margin(0.001));
  }

  SECTION("picking among many lights converges to the same answer as sampling all")
  {
    auto test = TestScene{};
    for (auto i = 0; i < 40; ++i)
    {
      auto light = TestScene::makePointLight(30.0f, PreviewAttenuation::None);
      light.origin = vm::vec3f{float(i % 7) * 3.0f, float(i / 7) * 3.0f, 100.0f};
      test.scene.lights.push_back(light);
    }
    test.finish();

    auto camera = PreviewCamera{};
    camera.position = vm::vec3f{0, 0, 200};
    camera.forward = vm::vec3f{0, 0, -1};
    camera.right = vm::vec3f{1, 0, 0};
    camera.up = vm::vec3f{0, 1, 0};
    camera.halfWidth = 0.0002f;
    camera.halfHeight = 0.0002f;
    camera.width = 1;
    camera.height = 1;

    const auto average = [&](const PreviewTraceSettings& settings, const int samples) {
      auto sum = vm::vec3f{0, 0, 0};
      for (auto i = 0; i < samples; ++i)
      {
        sum = sum + tracePreviewPixel(test.scene, camera, settings, 0, 0, uint32_t(i));
      }
      return (sum / float(samples)).x();
    };

    auto all = PreviewTraceSettings{};
    all.maxBounces = 0;
    all.maxShadowRays = 64;

    auto few = PreviewTraceSettings{};
    few.maxBounces = 0;
    few.maxShadowRays = 4;

    CHECK(average(few, 20000) == Catch::Approx(average(all, 4000)).margin(0.05));
  }
}

} // namespace tb::render
