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

  void addQuad(
    const vm::vec3f& origin,
    const vm::vec3f& edgeU,
    const vm::vec3f& edgeV,
    const vm::vec3f& normal,
    const PreviewSurfaceKind kind = PreviewSurfaceKind::Solid,
    const vm::vec3f& emission = vm::vec3f{0, 0, 0})
  {
    const auto addTriangle =
      [&](const vm::vec3f& p0, const vm::vec3f& p1, const vm::vec3f& p2) {
        auto shading = PreviewTriangleShading{};
        shading.normal = normal;
        shading.materialIndex = white;
        shading.kind = kind;
        shading.occludes = kind == PreviewSurfaceKind::Solid;
        shading.emission = emission;

        scene.trianglePositions.push_back(PreviewTrianglePos{p0, p1 - p0, p2 - p0});
        scene.triangleShading.push_back(shading);
      };

    addTriangle(origin, origin + edgeU, origin + edgeU + edgeV);
    addTriangle(origin, origin + edgeU + edgeV, origin + edgeV);
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

} // namespace

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
    auto test = TestScene{};
    auto light = TestScene::makePointLight(300.0f, PreviewAttenuation::None);
    light.origin = vm::vec3f{100, 0, 100};
    light.angleScale = 0.5f;
    test.scene.lights.push_back(light);
    test.finish();

    const auto cosTheta = 1.0f / std::sqrt(2.0f);
    CHECK(
      test.shade(256)
      == Catch::Approx(display(300.0f * (0.5f + 0.5f * cosTheta))).margin(0.02));
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
