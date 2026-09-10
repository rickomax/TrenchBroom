# Light Preview

`render::LightPreview` path traces the map's lighting and draws the result over the 3D
view. It refines itself pass by pass on its own threads, and starts over whenever the
camera moves or the map changes, so the editor never waits for it.

The pieces:

| File | What it does |
| --- | --- |
| `LightPreviewLights` | Normalizes every light entity and worldspawn key into `PreviewLight` |
| `LightPreviewScene` | Collects the map's triangles, albedo and emitters into a `PreviewScene` |
| `LightPreviewBvh` | Binned-SAH bounding volume hierarchy, closest hit and any hit |
| `LightPreviewTracer` | The path tracing kernel: one path per call |
| `LightPreview` | Worker pool, accumulation buffer, GL overlay, invalidation |

## What the preview reproduces

Point, spot, sun and surface lights; the sky and ground domes; global, per brush model
and `delay 4` local minimum light; projected texture lights. All five `delay` attenuation
formulas, `wait`, `_falloff`, `_anglescale`, `_deviance`, `_softangle`, the GoldSrc
`_cone`/`_cone2` cone spelling, `_light_channel_mask`, negative lights, and the
`_dist`/`_range`/`_gamma`/`_maxlight` worldspawn controls. Indirect light, with
`_bounce`, `_bouncescale` and `_bouncecolorscale`, which like the compilers is off unless
the map turns it on; the "Indirect light" setting in View Options overrides that the same
way passing `-bounce` does.

Brush models cast shadows only where the compilers do: the world and the brushes merged
into it by `func_detail` and `func_group` always cast unless `_shadow` `-1` says
otherwise, and a separate brush model casts only when `_shadow` `1` asks it to.

Faces the game config tags transparent (water, slime, lava, triggers, clip, hint) are
shaded but do not block light, because the compiler only traces against the solid hull. A
texture whose name starts with an asterisk counts as a liquid whether or not the game
config carries a tag for it. Liquids are also drawn as see-through as the editor draws
them, at the "Transparent faces" alpha, so a ray carries on to whatever is under the
water even though light ignores the surface entirely.

Faces the game never draws are left out of the scene altogether: triggers, clip, skip,
hint, caulk, nodraw, origin and the portal shaders. They are not drawn, they do not block
light, and they do not colour the light that bounces around them. What counts as one is
decided by the game config's `nodraw`, `hint` and `skip` surface flags, its `playerclip`,
`monsterclip` and `origin` content flags, Quake 3 surface parameters, the classname for
trigger entities, and the handful of texture names the whole family uses for faces that
exist only for the compiler.

Sky faces are a hole into the sky rather than a surface.

Entity models are lit along with the brushwork, under the "Include entity models" toggle
in View Options. They do not cast shadows, which is what the compilers do: a model entity
is not part of the BSP, so nothing about it reaches the lightmap, and a preview that let
one cast a shadow would show something the compiled map will not have.

## Where it deliberately differs, and what is missing

These are worth revisiting; they are listed here rather than buried in the code so that
they can be checked against a real compile.

**Open questions**

- *`_range` is treated as defaulting to 1.* The manual describes it as "values of n > 0.5
  makes lights brighter and n < 0.5 makes lights less bright", which reads as though the
  default were 0.5, but it does not state one. Reading it that way would halve every
  preview, so it is left at 1 until something settles it.

**Read from the wrong place, or guessed**

- *Nothing in an FGD says whether a class is drawn.* An entity definition gives the
  classname, whether the class is solid or a point, and its properties, and that is all, so
  which brush entities to leave out is decided from the classname and from the game config
  rather than from the definition. In practice that means trigger classes, which every game
  in the family spells the same way. The texture names alongside them are conventions
  rather than anything a game states, and are the first place to look when a game previews
  something it should not.

- *GoldSrc attenuation is inferred from the key spelling.* An entity that writes `_light`
  rather than `light` is previewed with inverse square falloff, since that is what HLRAD
  does, while ericw-tools falls off linearly. The game configuration cannot say which
  compiler a map is for, so the spelling stands in for it.
- *Sky faces are a plain hole.* Games lay sky textures out in their own way and none of
  that is reproduced. A ray that hits sky picks up the dome radiance; the camera sees the
  sky texture's average colour. Which faces count as sky is decided by a surface flag
  named `sky`, a Quake 3 `sky` surface parameter, or a texture named `sky` something.
- *A ray that escapes the map picks up sky light.* In a sealed map this never happens. In
  a leaking one it lights the leak, where a compiler would not.
- *Animated lights preview at the average brightness of their style,* rather than at
  whichever frame the clock is on, so that a flickering torch does not make the preview
  flicker. `spawnflags` "start off" is ignored: the light is previewed lit, because a
  mapper switching the preview on usually wants to see where it lands.

- *A model contributes its shape and one colour, not its skin.* The geometry comes from
  the triangles a model frame keeps for hit testing, which carry no UV coordinates, so the
  albedo is the average colour of the skin the frame would be drawn with. Getting the real
  skin onto a model means exposing the mesh vertex data, which `EntityModelMesh` currently
  keeps to itself.
- *Sprites are left out.* Anything whose orientation is not `Oriented` is turned to face
  the camera as it is drawn and has no fixed shape to trace against.

**Not implemented**

- Dirtmapping: `_dirt`, `_dirtmode`, `_dirtdepth`, `_dirtscale`, `_dirtgain`,
  `_dirtangle` and the per light and per sun overrides.
- Phong shading: `_phong`, `_phong_angle`, `_phong_angle_concave`, `_phong_group`. Brush
  faces are shaded flat and patches use their grid normals.
- Shadow keys beyond `_shadow` and `_nostaticlight`: `_shadowself`, `_shadowworldonly`,
  `_switchableshadow`, `_switchableshadow_target`, `_light_twosided`, `_light_alpha`.
- The per brush model surface light keys `_surflight_color`, `_surflight_style`,
  `_surflight_targetname`, `_surflight_rescale`, `_surflight_atten` and
  `_surflight_minlight_scale`. `_surflight_group` is parsed but not yet matched against
  brush models.
- `_minlight_mottle`, `_minlight_exclude`, `_autominlight` and `_autominlight_target`.
- `_lightmap_scale`, `_compilerstyle_start`, `_compilerstyle_max` and `_bouncestyled`,
  which describe how the lightmap is written rather than how light travels.

**Deliberately not applicable**

- `_samples` on a `_deviance` light. The preview takes one sample of the light's sphere
  per pass and the passes converge to the same answer, so the count has nothing to do.
  The manual notes that `light` is scaled down for most formulas to keep the brightness
  equal as `_deviance` splits it; averaging the passes does that on its own.
- `_surflightsubdivision` and `-surflight_subdivide`. Surface lights are sampled as area
  lights rather than subdivided into point lights, so no spacing is chosen.
- `-soft`, `-extra` and `-extra4`, which oversample and smooth the lightmap. The preview
  jitters within each pixel and averages the passes, which is the same idea carried
  further.
- `_bouncelightsubdivision`, for the same reason.

## Notes on the units

A lightmap value of 255 is a fully lit white surface, which is why shading divides by 255
at the very end. Everything up to that point is in the same units the `light` key is in,
so that a light of 300 at 100 units with the default linear falloff reads 200, exactly as
it would in the compiled lightmap. Surface light emission and the sky domes are converted
into the same scale when they are collected, so that a `_surface` light of 180 and a point
light of 180 land a facing surface in the same place.

Albedo is kept in gamma space rather than linearized, because that is the space the
compilers bounce light in, and matching them is the point.
