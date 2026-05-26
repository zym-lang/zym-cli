# `UI.anim*`

ImAnim animation surface exposed under the same global `UI` namespace
as the Dear ImGui widgets (see [`ui.md`](ui.md)) and the ImPlot
plotting surface (see [`ui.plot.md`](ui.plot.md)). ImAnim rides on
top of ImGui — it stores per-channel state inside the active ImGui
context via `ImPool` + `ImGuiStorage` keyed by `ImGuiID` — so every
`UI.anim*` call must happen inside a `UI.frame(win, body)` body.

ImAnim is immediate-mode like ImGui: scripts call `UI.animTween*` /
`UI.animOscillate*` / etc. every frame with the current target value
and a `dt`, and the bridge advances the channel internally. The
return value is the current sampled value for that channel.

---

## Conventions

- **Frame guard.** All `UI.anim*` calls except `UI.animSetAutoFrameUpdate`
  and `UI.animIsAutoFrameUpdateEnabled` require an active ImGui frame.
  They must be reached from inside `UI.frame(win, body)`. Calls
  outside that scope raise a runtime error of the form
  `ui.animX: called outside ui.frame(...)`.
- **Auto frame update.** `ui.frame(...)` automatically calls
  `iam_update_begin_frame()` and `iam_clip_update(dt)` once per
  frame using `ImGui::GetIO().DeltaTime`. Scripts that want manual
  control (scrub, pause, multi-pass, deterministic replay) call
  `UI.animSetAutoFrameUpdate(enable)` and then drive both
  `UI.animUpdateBeginFrame()` and `UI.animClipUpdate(dt)` themselves
  every frame. The single flag gates both calls — there's no
  realistic scenario where you'd want one auto and the other manual
  since the clip system feeds off the tween system.
- **Channel IDs.** The **tween** family (`animTween*`, `animRebase*`)
  takes both an `id` (the "owner" — typically a widget label or
  window name) and a `channelId` (which logical animation on that
  owner). The **oscillate / shake / wiggle / noise / drag** families
  do **not** take a `channelId` — they are addressed by `id` alone.
  IDs can be passed as **strings** (hashed via `ImHashStr` to the
  same `ImGuiID` ImGui would assign to a widget with that label) or
  as **raw numbers** if the script has cached an `ImGuiID` from a
  prior call. Mixing forms across calls is safe — the hash is stable.
- **Time.** Durations (`dur`) and per-call delta-time (`dt`) are
  seconds. `dt` is supplied per call so scripts can independently
  slow-mo / fast-forward / pause individual channels.
- **Ease descriptors.** Every animation call takes an `ease`
  argument that is either:
    - A `UI.ANIM_EASE_*` integer preset (e.g. `UI.ANIM_EASE_OUT_CUBIC`).
    - A 5-element list `[type, p0, p1, p2, p3]` produced by one of the
      `UI.animEase*` constructors (`animEaseBezier`, `animEaseSteps`,
      `animEaseBack`, `animEaseElastic`, `animEaseSpring`,
      `animEaseCustom`). The constructors are pure value-builders —
      no native state, no GC roots.
- **Per-axis ease.** Vec2 / vec4 / color per-axis tweens take a
  list of ease descriptors (2 entries for vec2, 4 for vec4/color),
  each itself a preset int OR a 5-list.
- **Vectors.** `ImVec2` is a 2-element list `[x, y]`; `ImVec4` is a
  4-element list `[x, y, z, w]`. Color tweens use ImVec4 in normalised
  `[r, g, b, a]` range with an explicit `colorSpace` (`UI.ANIM_COL_*`).
- **Policies.** When a fresh tween call targets a channel that's
  already animating, the `policy` (`UI.ANIM_POLICY_*`) decides what
  happens: `CROSSFADE` blends, `CUT` snaps to the new target, `QUEUE`
  waits for the current animation to finish.
- **Relative tweens.** `*Rel` variants take a `percent` of an
  `anchorSpace` (`UI.ANIM_ANCHOR_*` — window content / window /
  viewport / last item) plus an optional pixel bias. Use these for
  layout-independent animations that resize with the host.
- **Color spaces.** Color tweens / blends take a `colorSpace` arg
  (`UI.ANIM_COL_SRGB`, `_SRGB_LINEAR`, `_HSV`, `_OKLAB`, `_OKLCH`).
  `OKLAB` / `OKLCH` give perceptually-uniform color interpolation.
- **Opaque builders.** Path and gradient construction goes through
  a procedural "active builder" surface (`animPathBegin` ...
  `animPathEnd`, `animGradientBegin` ... `animGradientEnd`) keyed by
  an `ImGuiID` the script supplies. Only one path / gradient can be
  under construction at a time per kind; starting a new `Begin`
  drops any previous unfinished builder so scripts can't leak.
  Once `End` is called the builder is persisted under the supplied
  ID and is queryable / animatable by that ID.
- **Clip authoring.** `animClipBegin(clipId)` ... `animClipKey*` ...
  `animClipEnd()` work the same way — a singleton active-clip
  builder is held until `End` and then registered with the clip
  system. `animPlay(clipId, instId)` instantiates a clip and
  returns an instance ID for `animInstance*` ops.
- **Errors.** As with `UI.*`, bad argument types raise a Zym runtime
  error of the form `ui.animX(...) expects a <type>`. `UI.lastError()`
  surfaces the most recent ImGui-side message.

---

## Frame / global tuning

| Method | Returns | Notes |
| --- | --- | --- |
| `UI.animUpdateBeginFrame()` | null | Per-frame bookkeeping. Auto-called by `UI.frame(...)` unless `animSetAutoFrameUpdate(false)`. |
| `UI.animClipUpdate(dt)` | null | Advance the clip system by `dt` seconds. Auto-called by `UI.frame(...)` unless `animSetAutoFrameUpdate(false)`. |
| `UI.animSetAutoFrameUpdate(enable)` | null | Toggle the auto-driver. Default `true`. When `false`, scripts must call both `animUpdateBeginFrame` and `animClipUpdate` themselves. |
| `UI.animIsAutoFrameUpdateEnabled()` | bool | Current value of the flag. Safe to call outside `UI.frame`. |
| `UI.animGc(maxAgeFrames)` | null | Drop stale tween entries older than `maxAgeFrames` (upstream default is 600). |
| `UI.animReserve(float, vec2, vec4, int, color)` | null | Pre-allocate per-channel pool capacity. Zero/negative values are ignored per channel. |
| `UI.animSetEaseLutSamples(count)` | null | LUT resolution for parametric easings (upstream clamps to >=9; default 256). |
| `UI.animSetGlobalTimeScale(scale)` | null | Multiplier applied to every channel's `dt`. |
| `UI.animGetGlobalTimeScale()` | number | Current global time scale. |
| `UI.animSetLazyInit(enable)` | null | Defer pool allocation until first tween. |
| `UI.animIsLazyInitEnabled()` | bool | Current lazy-init flag. |
| `UI.animEvalPreset(type, t)` | number | Stateless sample of a preset easing at `t` in `[0, 1]`. Parametric easings (bezier / spring / steps / custom) must be evaluated through the tween API. |

---

## Easing descriptors

Constructors return a 5-element list `[type, p0, p1, p2, p3]` that can
be passed as the `ease` arg to any tween / clip-key call. The list
form is interchangeable with a bare preset int.

| Method | Returns | Notes |
| --- | --- | --- |
| `UI.animEasePreset(type)` | list | Wraps a preset enum in the 5-list shape for uniform call shape across preset / parametric eases. |
| `UI.animEaseBezier(x1, y1, x2, y2)` | list | CSS-style cubic bezier. P0 and P3 are implicit `(0, 0)` and `(1, 1)`. |
| `UI.animEaseSteps(steps, jumpMode)` | list | `jumpMode`: `0` = jump-end (default), `1` = jump-start, `2` = jump-both. |
| `UI.animEaseBack(overshoot, dir)` | list | `dir`: `0` = in, `1` = out, `2` = inOut. `overshoot` follows the `c1` convention (≈ 1.70158 for default 10%). |
| `UI.animEaseElastic(amplitude, period, dir)` | list | `dir`: `0` = in, `1` = out, `2` = inOut. |
| `UI.animEaseSpring(mass, stiffness, damping, v0)` | list | Critically-damped feel ≈ `damping² ≈ 4 * mass * stiffness`. `v0` is initial velocity. |
| `UI.animEaseCustom(slot)` | list | Reference a function registered via the (currently unbound) `iam_register_custom_ease(slot, fn)`. Slot is `0..15`. Evaluates as no-op until a native registers a function. |
| `UI.animAnchorSize(space)` | `[w, h]` | Pixel size of the requested anchor (`UI.ANIM_ANCHOR_*`). Requires an active frame. |
| `UI.animGetBlendedColor(a, b, t, space)` | `[r, g, b, a]` | Stateless color blend. `a` / `b` are 4-element color lists, `t` in `[0, 1]`. |

---

## Tweens

Static-target tweens advance a channel toward `target` over `dur`
seconds, returning the current sampled value. Every call takes the
canonical `(id, channelId, target, dur, ease, policy, dt)` blob.

### Float / int / vec2 / vec4 / color

| Method | Returns | Notes |
| --- | --- | --- |
| `UI.animTweenFloat(id, channelId, target, dur, ease, policy, dt)` | number | |
| `UI.animTweenInt(id, channelId, target, dur, ease, policy, dt)` | int | |
| `UI.animTweenVec2(id, channelId, target, dur, ease, policy, dt)` | `[x, y]` | `target` is `[x, y]`. |
| `UI.animTweenVec4(id, channelId, target, dur, ease, policy, dt)` | `[x, y, z, w]` | `target` is `[x, y, z, w]`. |
| `UI.animTweenColor(id, channelId, target, dur, ease, policy, colorSpace, dt)` | `[r, g, b, a]` | `target` is a 4-element color list. |

### Relative (percent-of-anchor) variants

`percent` is in `[0, 1]`, `pxBias` is an additive pixel offset, and
`anchorSpace` is a `UI.ANIM_ANCHOR_*` constant.

| Method | Returns | Notes |
| --- | --- | --- |
| `UI.animTweenFloatRel(id, channelId, percent, pxBias, dur, ease, policy, anchorSpace, axis, dt)` | number | `axis`: `0` = width, `1` = height of the anchor. |
| `UI.animTweenVec2Rel(id, channelId, percent, pxBias, dur, ease, policy, anchorSpace, dt)` | `[x, y]` | `percent` and `pxBias` are 2-element lists. |
| `UI.animTweenVec4Rel(id, channelId, percent, pxBias, dur, ease, policy, anchorSpace, dt)` | `[x, y, z, w]` | `percent` and `pxBias` are 4-element lists. |
| `UI.animTweenColorRel(id, channelId, percent, pxBias, dur, ease, policy, colorSpace, anchorSpace, dt)` | `[r, g, b, a]` | |

### Per-axis variants

`ease` is a list of per-axis ease descriptors (2 entries for vec2, 4
for vec4 / color), each a preset int OR a 5-list.

| Method | Returns | Notes |
| --- | --- | --- |
| `UI.animTweenVec2PerAxis(id, channelId, target, dur, easeXY, policy, dt)` | `[x, y]` | |
| `UI.animTweenVec4PerAxis(id, channelId, target, dur, easeXYZW, policy, dt)` | `[x, y, z, w]` | |
| `UI.animTweenColorPerAxis(id, channelId, target, dur, easeRGBA, policy, colorSpace, dt)` | `[r, g, b, a]` | |

### Rebase

`rebase*` redirects the currently-running tween on a channel to a new
target while preserving accumulated time and easing — useful when the
underlying value changes mid-flight.

| Method | Returns | Notes |
| --- | --- | --- |
| `UI.animRebaseFloat(id, channelId, newTarget, dt)` | null | |
| `UI.animRebaseInt(id, channelId, newTarget, dt)` | null | |
| `UI.animRebaseVec2(id, channelId, newTarget, dt)` | null | |
| `UI.animRebaseVec4(id, channelId, newTarget, dt)` | null | |
| `UI.animRebaseColor(id, channelId, newTarget, dt)` | null | |

### Scroll helpers

Animated `ImGui` scroll wrappers. Call from inside the target window —
they act on the current window's scroll state, so they take no `id`.
They are driven by the auto frame update, so they take no `dt`.

| Method | Returns | Notes |
| --- | --- | --- |
| `UI.animScrollToY(targetY, dur, ease)` | null | |
| `UI.animScrollToX(targetX, dur, ease)` | null | |
| `UI.animScrollToTop(dur, ease)` | null | |
| `UI.animScrollToBottom(dur, ease)` | null | |

---

## Oscillate / shake / wiggle

Continuous waveform generators. Unlike the tween family, these are
addressed by `id` alone — there is no `channelId` argument. Oscillate /
shake / wiggle return the **raw perturbation** (offset or value); the
script adds its own base value. The color variants are the exception:
they take a `baseColor` and return a color perturbed around it. All
type variants within a family share the same call shape; parameters
differ by family.

### Oscillate (sustained periodic waveform)

| Method | Returns | Notes |
| --- | --- | --- |
| `UI.animOscillate(id, amplitude, frequency, waveType, phase, dt)` | number | `waveType` is `UI.ANIM_WAVE_*`. |
| `UI.animOscillateInt(id, amplitude, frequency, waveType, phase, dt)` | int | |
| `UI.animOscillateVec2(id, amplitude, frequency, waveType, phase, dt)` | `[x, y]` | |
| `UI.animOscillateVec4(id, amplitude, frequency, waveType, phase, dt)` | `[x, y, z, w]` | |
| `UI.animOscillateColor(id, baseColor, amplitude, frequency, waveType, phase, colorSpace, dt)` | `[r, g, b, a]` | |

### Shake (transient burst with decay)

Shakes are one-shot — call `animTriggerShake` to (re)start one, then
sample the current value every frame.

| Method | Returns | Notes |
| --- | --- | --- |
| `UI.animTriggerShake(id)` | null | (Re)start / re-arm the shake on `id`. Takes the id only — intensity / frequency / decay are supplied to `animShake`. |
| `UI.animShake(id, intensity, frequency, decayTime, dt)` | number | |
| `UI.animShakeInt(id, intensity, frequency, decayTime, dt)` | int | |
| `UI.animShakeVec2(id, intensity, frequency, decayTime, dt)` | `[x, y]` | |
| `UI.animShakeVec4(id, intensity, frequency, decayTime, dt)` | `[x, y, z, w]` | |
| `UI.animShakeColor(id, baseColor, intensity, frequency, decayTime, colorSpace, dt)` | `[r, g, b, a]` | |

### Wiggle (smoothed noise)

| Method | Returns | Notes |
| --- | --- | --- |
| `UI.animWiggle(id, amplitude, frequency, dt)` | number | |
| `UI.animWiggleInt(id, amplitude, frequency, dt)` | int | |
| `UI.animWiggleVec2(id, amplitude, frequency, dt)` | `[x, y]` | |
| `UI.animWiggleVec4(id, amplitude, frequency, dt)` | `[x, y, z, w]` | |
| `UI.animWiggleColor(id, baseColor, amplitude, frequency, colorSpace, dt)` | `[r, g, b, a]` | |

---

## Drag feedback

Inertial / snapping drag helpers. Drag channels are addressed by `id`
alone — there is no `channelId`. `animDragRelease` is the only call
that takes snap options; the rest are unconditional state transitions.

`animDragBegin`, `animDragUpdate` and `animDragRelease` all return the
same 6-element drag-state list:

```
[ [px, py], [ox, oy], [vx, vy], dragging, snapping, progress ]
```

where `px/py` is the current position, `ox/oy` is the offset from the
press origin, `vx/vy` is the current velocity, `dragging`/`snapping`
are bools, and `progress` is the snap-progress in `[0, 1]`.

| Method | Returns | Notes |
| --- | --- | --- |
| `UI.animDragBegin(id, pos)` | drag-state list | `pos` is `[x, y]`, the press position. |
| `UI.animDragUpdate(id, pos, dt)` | drag-state list | `pos` is the current pointer position. |
| `UI.animDragRelease(id, pos, snapGrid, snapPoints, snapDuration, overshoot, easeType, dt)` | drag-state list | `snapGrid` is `[w, h]` grid cell size (`[0, 0]` = no grid). `snapPoints` is `null` or a list of `[x, y]` custom snap targets. `overshoot` controls inertial overshoot; `easeType` is a `UI.ANIM_EASE_*` preset int. |
| `UI.animDragCancel(id)` | null | |

---

## Motion paths

Stateless curve evaluators plus a stateful "active builder" surface
for assembling multi-segment paths keyed by an `ImGuiID`. Builders are
singletons — only one path can be under construction at a time. Calling
`animPathBegin` while another build is in progress drops the previous
unfinished builder.

### Stateless curve evaluators

| Method | Returns | Notes |
| --- | --- | --- |
| `UI.animBezierQuadratic(p0, p1, p2, t)` | `[x, y]` | Points are `[x, y]` lists. |
| `UI.animBezierCubic(p0, p1, p2, p3, t)` | `[x, y]` | |
| `UI.animCatmullRom(p0, p1, p2, p3, t, tension)` | `[x, y]` | |
| `UI.animBezierQuadraticDeriv(p0, p1, p2, t)` | `[x, y]` | First derivative (tangent vector, unnormalised). |
| `UI.animBezierCubicDeriv(p0, p1, p2, p3, t)` | `[x, y]` | |
| `UI.animCatmullRomDeriv(p0, p1, p2, p3, t, tension)` | `[x, y]` | |

### Builder (procedural)

| Method | Returns | Notes |
| --- | --- | --- |
| `UI.animPathBegin(pathId, start)` | null | Starts a fresh builder under `id`. Drops any previous unfinished builder. `startPos` is `[x, y]`. |
| `UI.animPathLineTo(end)` | null | |
| `UI.animPathQuadraticTo(ctrl, end)` | null | Quadratic bezier: control `c`, endpoint `p`. |
| `UI.animPathCubicTo(ctrl1, ctrl2, end)` | null | Cubic bezier: control points `c1` / `c2`, endpoint `p`. |
| `UI.animPathCatmullTo(end, tension)` | null | Catmull-Rom with explicit control points. |
| `UI.animPathClose()` | null | Closes the contour back to the start. |
| `UI.animPathEnd()` | null | Registers the path under the ID supplied to `animPathBegin`. |

### Queries

| Method | Returns | Notes |
| --- | --- | --- |
| `UI.animPathExists(pathId)` | bool | |
| `UI.animPathLength(pathId)` | number | Total parametric length (sum of segment count, **not** arc length — see arc helpers). |
| `UI.animPathEvaluate(pathId, t)` | `[x, y]` | `t` in `[0, 1]`. Parametric (uneven distribution along long curves). |
| `UI.animPathTangent(pathId, t)` | `[x, y]` | Unnormalised tangent vector. |
| `UI.animPathAngle(pathId, t)` | number | Radians. |

### Along-path tweens

| Method | Returns | Notes |
| --- | --- | --- |
| `UI.animTweenPath(id, channelId, pathId, dur, ease, policy, dt)` | `[x, y]` | Animates a point along a path. |
| `UI.animTweenPathAngle(id, channelId, pathId, dur, ease, policy, dt)` | number | Animates the tangent angle along a path (radians). |

### Arc-length parameterisation

Even-distance traversal of curves. Build the LUT once with
`animPathBuildArcLut`, then query distances instead of parametric `t`.

| Method | Returns | Notes |
| --- | --- | --- |
| `UI.animPathBuildArcLut(pathId, subdivisions)` | null | Build / rebuild the LUT for the path. |
| `UI.animPathHasArcLut(pathId)` | bool | |
| `UI.animPathDistanceToT(pathId, distance)` | number | Map distance → parametric `t`. |
| `UI.animPathEvaluateAtDistance(pathId, distance)` | `[x, y]` | |
| `UI.animPathAngleAtDistance(pathId, distance)` | number | Radians. |
| `UI.animPathTangentAtDistance(pathId, distance)` | `[x, y]` | |

---

## Path morphing + text-along-path + quad transforms + text stagger

### Path morphing

Blend between two registered paths.

| Method | Returns | Notes |
| --- | --- | --- |
| `UI.animPathMorph(pathA, pathB, t, blend, opts)` | `[x, y]` | `t` is the parametric position in `[0, 1]`; `blend` in `[0, 1]` is the A→B morph factor. `opts` is `null` or a 3-element list `[samples, matchEndpoints, useArcLength]` (int, bool, bool). |
| `UI.animPathMorphTangent(pathA, pathB, t, blend, opts)` | `[x, y]` | |
| `UI.animPathMorphAngle(pathA, pathB, t, blend, opts)` | number | Radians. |
| `UI.animTweenPathMorph(id, channelId, pathA, pathB, targetBlend, dur, pathEase, morphEase, policy, dt, opts)` | `[x, y]` | Animates the blend factor and samples the morphed path. Variadic: `dt` is required, `opts` optional (omit or `null` for defaults). The native closure signature parser caps fixed params at 9 when `...` is used, so `dt` / `opts` ride the variadic tail. |
| `UI.animGetMorphBlend(id, channelId)` | number | Current blend factor for an in-progress morph. |

### Text along path

Draw text along a registered path. Colors are 4-element color lists
internally packed to `ImU32`. The active font is whatever ImGui has
pushed at call time (use `UI.withFont` to override).

**Text-path opts.** The `opts` argument is `null` or a 4-element list
`[offset, letterSpacing, align, flipY]`, where `offset` is the start
position along the path in `[0, 1]`, `letterSpacing` is extra pixel
spacing per glyph, `align` is a `UI.ANIM_TEXT_ALIGN_*` constant, and
`flipY` is a bool.

| Method | Returns | Notes |
| --- | --- | --- |
| `UI.animTextPath(pathId, text, opts)` | null | Draws `text` along the registered path `pathId`. |
| `UI.animTextPathAnimated(pathId, text, progress, opts)` | null | `progress` in `[0, 1]` drives a reveal / effect sweep. |
| `UI.animTextPathWidth(text, opts)` | number | Pixel width of `text` in the active font with the opts' letter spacing applied. |

### Quad transforms

| Method | Returns | Notes |
| --- | --- | --- |
| `UI.animTransformQuad(quad, center, angleRad, translation)` | quad | `quad` is a 4-list of `[x, y]` vertices. Rotates the quad by `angleRad` around `center` (`[x, y]`), then adds `translation` (`[x, y]`). No scale — bake scale into the quad's vertices. |
| `UI.animMakeGlyphQuad(pos, angleRad, glyphW, glyphH, baselineOffset)` | quad | Build a 4-vertex quad of size `glyphW` × `glyphH` at `pos` (`[x, y]`), rotated by `angleRad`, with `baselineOffset` shifting it along the baseline. |

### Text stagger

Animated per-glyph entrance effects without a path.

**Text-stagger opts.** The `opts` argument is `null` or a 9-element
list `[pos, effect, charDelay, charDuration, effectIntensity, ease,
color, fontScale, letterSpacing]`: `pos` is `[x, y]`, `effect` is a
`UI.ANIM_TEXT_FX_*` constant, `charDelay` is the per-glyph stagger
offset, `charDuration` is each glyph's effect duration, `ease` is an
ease descriptor (preset int or 5-list), `color` is `[r, g, b, a]`.

| Method | Returns | Notes |
| --- | --- | --- |
| `UI.animTextStagger(id, text, progress, opts)` | null | `progress` in `[0, 1]` advances the per-glyph sequence. |
| `UI.animTextStaggerWidth(text, opts)` | number | Pixel width with stagger letter spacing applied. |
| `UI.animTextStaggerDuration(text, opts)` | number | Total duration of the stagger sequence (seconds). |

---

## Noise

Pseudorandom channels backed by Perlin / simplex / value / worley
noise. `animNoise*` returns the raw noise value at a point;
`animNoiseChannel*` / `animSmoothNoise*` advance a channel-bound
value (addressed by `id` — no `channelId`).

**Noise opts.** The `opts` argument is `null` (defaults) or a
5-element list `[type, octaves, persistence, lacunarity, seed]`:
`type` is a `UI.ANIM_NOISE_*` constant, `octaves` is the fractal
layer count, `persistence` / `lacunarity` are the per-octave
amplitude / frequency multipliers, and `seed` is an int for
reproducible output. `animSmoothNoise*` does not take an `opts`
list — it is a fixed low-pass-filtered noise channel.

### Stateless noise

| Method | Returns | Notes |
| --- | --- | --- |
| `UI.animNoise2d(x, y, opts)` | number | Value in `[-1, 1]`. `opts` is the noise-opts list (see above). |
| `UI.animNoise3d(x, y, z, opts)` | number | |

### Noise channel (fractal noise around a base)

| Method | Returns | Notes |
| --- | --- | --- |
| `UI.animNoiseChannelFloat(id, freq, amp, opts, dt)` | number | `freq` scales the sample rate, `amp` scales the output. |
| `UI.animNoiseChannelVec2(id, freq, amp, opts, dt)` | `[x, y]` | |
| `UI.animNoiseChannelVec4(id, freq, amp, opts, dt)` | `[x, y, z, w]` | |
| `UI.animNoiseChannelColor(id, baseColor, amp, freq, opts, space, dt)` | `[r, g, b, a]` | Note the `amp` / `freq` order differs from the non-color variants. `space` is a `UI.ANIM_COL_*` constant. |

### Smooth noise (low-pass filtered)

| Method | Returns | Notes |
| --- | --- | --- |
| `UI.animSmoothNoiseFloat(id, amp, speed, dt)` | number | `speed` advances the noise field over time. |
| `UI.animSmoothNoiseVec2(id, amp, speed, dt)` | `[x, y]` | |
| `UI.animSmoothNoiseVec4(id, amp, speed, dt)` | `[x, y, z, w]` | |
| `UI.animSmoothNoiseColor(id, baseColor, amp, speed, space, dt)` | `[r, g, b, a]` | `space` is a `UI.ANIM_COL_*` constant. |

---

## Style interpolation

Snapshot and animate the current `ImGuiStyle`. The full `ImGuiStyle*`
struct is not modelled in Zym — interpolation runs against snapshots
the user registered via `animStyleRegisterCurrent`.

| Method | Returns | Notes |
| --- | --- | --- |
| `UI.animStyleRegisterCurrent(styleId)` | null | Snapshot the current `ImGuiStyle` under `id`. |
| `UI.animStyleBlend(styleA, styleB, t, colorSpace)` | null | Apply a blend of two registered styles to the live `ImGuiStyle`. |
| `UI.animStyleTween(id, targetStyle, dur, ease, colorSpace, dt)` | null | Animated style blend; mutates the live `ImGuiStyle` each frame. |
| `UI.animStyleExists(styleId)` | bool | |
| `UI.animStyleUnregister(styleId)` | null | |

---

## Gradient interpolation

Gradients are constructed via a singleton active builder (mirroring
the path builder) and persisted under an `ImGuiID`.

### Builder

| Method | Returns | Notes |
| --- | --- | --- |
| `UI.animGradientBegin(gradientId)` | null | Starts a fresh builder; drops any previous unfinished builder. |
| `UI.animGradientAddStop(position, color)` | null | `position` in `[0, 1]`; `color` is `[r, g, b, a]`. |
| `UI.animGradientEnd()` | null | Registers the gradient under the ID supplied to `animGradientBegin`. |

### Queries / interpolation

| Method | Returns | Notes |
| --- | --- | --- |
| `UI.animGradientExists(gradientId)` | bool | |
| `UI.animGradientStopCount(gradientId)` | int | |
| `UI.animGradientSample(gradientId, t, colorSpace)` | `[r, g, b, a]` | Sample a gradient at `t`. |
| `UI.animGradientLerp(gradientA, gradientB, t, colorSpace, outGradientId)` | `[r, g, b, a]` | Blend two gradients then sample at `t`. |
| `UI.animTweenGradient(id, channelId, targetGradientId, dur, ease, policy, colorSpace, dt, outGradientId)` | `[r, g, b, a]` | Animates the blend factor between two gradients. |

---

## Transform interpolation

Transforms are 5-element lists `[posX, posY, scaleX, scaleY, rotRad]`.

| Method | Returns | Notes |
| --- | --- | --- |
| `UI.animTransformLerp(a, b, t, rotationMode)` | transform | `rotationMode` is `UI.ANIM_ROTATION_*` (controls rotation interpolation path). |
| `UI.animTweenTransform(id, channelId, target, dur, ease, policy, rotationMode, dt)` | transform | |
| `UI.animTransformApply(transform, point)` | `[x, y]` | Apply a transform to a point. |
| `UI.animTransformInverse(transform)` | transform | |
| `UI.animTransformCompose(a, b)` | transform | `a` then `b` (i.e. `b * a` matrix order). |
| `UI.animTransformFromMatrix(m00, m01, m10, m11, tx, ty)` | transform | |
| `UI.animTransformToMatrix(transform)` | `[m00, m01, m10, m11, tx, ty]` | |

---

## Variation descriptors

Per-iteration variation applied by clips with `keyFloat*Var` /
`set*Var` calls. Variation is fully data-driven: callbacks
(`UI.ANIM_VAR_CALLBACK`) are **not** bound — see the deferred-callback
TODO at the top of `src/natives/ui/imanim.cpp`. The data-only modes
(`UI.ANIM_VAR_NONE` through `_PINGPONG`) cover every realistic use case
once combined with per-axis decomposition, `min` / `max` clamps, and a
deterministic `seed`.

| Method | Returns | Notes |
| --- | --- | --- |
| `UI.animVarFloat(mode, amount, minClamp, maxClamp, seed)` | variation desc | `mode` is `UI.ANIM_VAR_*`. Non-zero `seed` makes random modes reproducible per iteration. |
| `UI.animVarInt(mode, amount, minClamp, maxClamp, seed)` | variation desc | |
| `UI.animVarVec2(mode, amount, minClamp, maxClamp, seed)` | variation desc | `amount` / clamps are 2-lists. |
| `UI.animVarVec4(mode, amount, minClamp, maxClamp, seed)` | variation desc | `amount` / clamps are 4-lists. |
| `UI.animVarColor(mode, amount, minClamp, maxClamp, colorSpace, seed)` | variation desc | |

---

## Clips

A clip is a keyframed timeline that can be instantiated (played) any
number of times. Authoring uses a singleton active-clip builder: open
with `animClipBegin(clipId)`, push keys / sequencing / loops, close
with `animClipEnd()`.

### Clip-system lifecycle

| Method | Returns | Notes |
| --- | --- | --- |
| `UI.animClipInit(initialClipCap, initialInstCap)` | null | Optional pre-allocate. |
| `UI.animClipShutdown()` | null | Drop all clips / instances. Also discards any in-progress builder. |
| `UI.animClipGc(maxAgeFrames)` | null | Drop stale clip / instance entries. |
| `UI.animClipExists(clipId)` | bool | |
| `UI.animClipDuration(clipId)` | number | Total clip length in seconds. |
| `UI.animClipSave(clipId, path)` | int | Result code (`UI.ANIM_OK` = 0, else `UI.ANIM_ERR_*`). |
| `UI.animClipLoad(path)` | `[resultCode, clipIdOrZero]` | |

### Authoring (active builder)

| Method | Returns | Notes |
| --- | --- | --- |
| `UI.animClipBegin(clipId)` | null | Starts a fresh builder; drops any previous unfinished builder. |
| `UI.animClipEnd()` | null | Registers the clip under the ID supplied to `animClipBegin`. |
| `UI.animClipSeqBegin()` | null | Open a sub-sequence (next siblings run in order). |
| `UI.animClipSeqEnd()` | null | |
| `UI.animClipParBegin()` | null | Open a parallel block (next siblings run together). |
| `UI.animClipParEnd()` | null | |
| `UI.animClipSetLoop(loop, direction, loopCount)` | null | `loop` is a bool (enable looping); `direction` is `UI.ANIM_DIR_*`; `loopCount` is the repeat count (`0` = infinite). |
| `UI.animClipSetDelay(delaySeconds)` | null | Pre-roll before the first key fires. |
| `UI.animClipSetStagger(count, eachDelay, fromCenterBias)` | null | `count` child blocks staggered by `eachDelay` seconds each. `fromCenterBias` biases the stagger origin toward the center of the set. |
| `UI.animClipSetDurationVar(varFloat)` | null | Per-iteration duration variation. |
| `UI.animClipSetDelayVar(varFloat)` | null | Per-iteration delay variation. |
| `UI.animClipSetTimescaleVar(varFloat)` | null | Per-iteration timescale variation. |

### Keyframes

`bezier4` is `null` or `[x1, y1, x2, y2]` and overrides `easeType`
when supplied. `easeType` is a preset int (`UI.ANIM_EASE_*`).

| Method | Returns | Notes |
| --- | --- | --- |
| `UI.animClipKeyFloat(channelId, time, value, easeType, bezier4)` | null | |
| `UI.animClipKeyInt(channelId, time, value, easeType)` | null | |
| `UI.animClipKeyVec2(channelId, time, value, easeType, bezier4)` | null | |
| `UI.animClipKeyVec4(channelId, time, value, easeType, bezier4)` | null | |
| `UI.animClipKeyColor(channelId, time, value, colorSpace, easeType, bezier4)` | null | |
| `UI.animClipKeyFloatSpring(channelId, time, target, mass, stiffness, damping, v0)` | null | Spring-driven key (no `easeType` needed). |
| `UI.animClipKeyFloatVar(channelId, time, value, varFloat, easeType, bezier4)` | null | With per-iteration value variation. |
| `UI.animClipKeyIntVar(channelId, time, value, varInt, easeType)` | null | |
| `UI.animClipKeyVec2Var(channelId, time, value, varVec2, easeType, bezier4)` | null | |
| `UI.animClipKeyVec4Var(channelId, time, value, varVec4, easeType, bezier4)` | null | |
| `UI.animClipKeyColorVar(channelId, time, value, varColor, colorSpace, easeType, bezier4)` | null | |
| `UI.animClipKeyFloatRel(channelId, time, percent, pxBias, anchorSpace, axis, easeType, bezier4)` | null | Relative key (resolves at play time against the anchor). |
| `UI.animClipKeyVec2Rel(channelId, time, percent, pxBias, anchorSpace, easeType, bezier4)` | null | |
| `UI.animClipKeyVec4Rel(channelId, time, percent, pxBias, anchorSpace, easeType, bezier4)` | null | |
| `UI.animClipKeyColorRel(channelId, time, percent, pxBias, colorSpace, anchorSpace, easeType, bezier4)` | null | |

---

## Instances

Playing a clip returns an instance ID. Instance ops accept the ID and
silently no-op on a missing/destroyed instance (upstream behaviour).

### Playback

| Method | Returns | Notes |
| --- | --- | --- |
| `UI.animPlay(clipId, instanceId)` | int | Start a new instance. `instanceId` may be `0` to autogenerate. Returns the instance ID. |
| `UI.animPlayStagger(clipId, instanceId, index)` | int | Same, with a per-index stagger offset applied. |
| `UI.animStaggerDelay(clipId, index)` | number | Pre-compute the stagger delay for a given index. |
| `UI.animGetInstance(instanceId)` | int | Returns the ID if the instance exists, else `0`. |

### State / control

| Method | Returns | Notes |
| --- | --- | --- |
| `UI.animInstancePause(id)` | null | |
| `UI.animInstanceResume(id)` | null | |
| `UI.animInstanceStop(id)` | null | Halts playback; instance stays alive until GC. |
| `UI.animInstanceDestroy(id)` | null | Tear down immediately. |
| `UI.animInstanceSeek(id, time)` | null | |
| `UI.animInstanceSetTimeScale(id, scale)` | null | Per-instance time scale (multiplies global). |
| `UI.animInstanceSetWeight(id, weight)` | null | Layered-blend weight; see Layering. |
| `UI.animInstanceThen(id, nextClipId, nextInstanceId)` | null | Queue another clip after this one. `nextInstanceId` may be `null` to autogenerate. |
| `UI.animInstanceThenDelay(id, delay)` | null | Add a delay to the queued-next clip. |
| `UI.animInstanceValid(id)` | bool | |
| `UI.animInstanceIsPlaying(id)` | bool | |
| `UI.animInstanceIsPaused(id)` | bool | |
| `UI.animInstanceTime(id)` | number | Current playhead time. |
| `UI.animInstanceDuration(id)` | number | Effective duration for this instance (after timescale / variation). |

### Channel-value queries

Return the current sampled value for a channel, or `null` if the
instance/channel doesn't exist.

| Method | Returns | Notes |
| --- | --- | --- |
| `UI.animInstanceGetFloat(id, channelId)` | number / null | |
| `UI.animInstanceGetInt(id, channelId)` | int / null | |
| `UI.animInstanceGetVec2(id, channelId)` | `[x, y]` / null | |
| `UI.animInstanceGetVec4(id, channelId)` | `[x, y, z, w]` / null | |
| `UI.animInstanceGetColor(id, channelId, colorSpace)` | `[r, g, b, a]` / null | |

---

## Layered blending

Blend multiple instances into a target. `animLayerBegin` /
`animLayerAdd` / `animLayerEnd` is the procedural form of the
upstream layered-blend block; the `animGetBlended*` queries read the
blended output channel from the target instance.

| Method | Returns | Notes |
| --- | --- | --- |
| `UI.animLayerBegin(targetInstanceId)` | null | Open the layer block for the target. |
| `UI.animLayerAdd(srcInstanceId, weight)` | null | Add a source layer at the given weight. |
| `UI.animLayerEnd(targetInstanceId)` | null | Commit the layered output. |
| `UI.animGetBlendedFloat(instanceId, channelId)` | number / null | |
| `UI.animGetBlendedInt(instanceId, channelId)` | int / null | |
| `UI.animGetBlendedVec2(instanceId, channelId)` | `[x, y]` / null | |
| `UI.animGetBlendedVec4(instanceId, channelId)` | `[x, y, z, w]` / null | |

---

## Constants reference

### `UI.ANIM_EASE_*` (preset easings — `iam_ease_type`)

`LINEAR`, `IN_QUAD`, `OUT_QUAD`, `IN_OUT_QUAD`, `IN_CUBIC`, `OUT_CUBIC`,
`IN_OUT_CUBIC`, `IN_QUART`, `OUT_QUART`, `IN_OUT_QUART`, `IN_QUINT`,
`OUT_QUINT`, `IN_OUT_QUINT`, `IN_SINE`, `OUT_SINE`, `IN_OUT_SINE`,
`IN_EXPO`, `OUT_EXPO`, `IN_OUT_EXPO`, `IN_CIRC`, `OUT_CIRC`,
`IN_OUT_CIRC`, `IN_BACK`, `OUT_BACK`, `IN_OUT_BACK`, `IN_ELASTIC`,
`OUT_ELASTIC`, `IN_OUT_ELASTIC`, `IN_BOUNCE`, `OUT_BOUNCE`,
`IN_OUT_BOUNCE`, `STEPS`, `CUBIC_BEZIER`, `SPRING`, `CUSTOM`.

### `UI.ANIM_POLICY_*` (retarget policy — `iam_policy`)

| Constant | Effect |
| --- | --- |
| `ANIM_POLICY_CROSSFADE` | Blend from the current sample into the new target. |
| `ANIM_POLICY_CUT` | Snap to the new target's starting value. |
| `ANIM_POLICY_QUEUE` | Wait for the current animation to finish, then run the new one. |

### `UI.ANIM_COL_*` (color spaces — `iam_color_space`)

| Constant | Meaning |
| --- | --- |
| `ANIM_COL_SRGB` | Standard sRGB. |
| `ANIM_COL_SRGB_LINEAR` | Linear sRGB. |
| `ANIM_COL_HSV` | Hue / saturation / value. |
| `ANIM_COL_OKLAB` | Perceptually-uniform Lab. |
| `ANIM_COL_OKLCH` | Perceptually-uniform LCH (polar OKLAB). |

### `UI.ANIM_ANCHOR_*` (anchor spaces — `iam_anchor_space`)

| Constant | Meaning |
| --- | --- |
| `ANIM_ANCHOR_WINDOW_CONTENT` | Inner content rect of the current window. |
| `ANIM_ANCHOR_WINDOW` | Full client rect of the current window. |
| `ANIM_ANCHOR_VIEWPORT` | Host viewport. |
| `ANIM_ANCHOR_LAST_ITEM` | Bounding rect of the most recent ImGui item. |

### `UI.ANIM_WAVE_*` (oscillator waveforms — `iam_wave_type`)

`SINE`, `TRIANGLE`, `SAWTOOTH`, `SQUARE`.

### `UI.ANIM_SEG_*` (path segment kinds — `iam_path_segment_type`)

`LINE`, `QUADRATIC_BEZIER`, `CUBIC_BEZIER`, `CATMULL_ROM`.

### `UI.ANIM_TEXT_ALIGN_*` (text-along-path alignment)

`START`, `CENTER`, `END`.

### `UI.ANIM_TEXT_FX_*` (text animation effects)

`NONE`, `FADE`, `SCALE`, `SLIDE_UP`, `SLIDE_DOWN`, `SLIDE_LEFT`,
`SLIDE_RIGHT`, `ROTATE`, `BOUNCE`, `WAVE`, `TYPEWRITER`.

### `UI.ANIM_NOISE_*` (noise types — `iam_noise_type`)

`PERLIN`, `SIMPLEX`, `VALUE`, `WORLEY`.

### `UI.ANIM_ROTATION_*` (rotation interpolation — `iam_rotation_mode`)

| Constant | Meaning |
| --- | --- |
| `ANIM_ROTATION_SHORTEST` | Take the shortest arc. |
| `ANIM_ROTATION_LONGEST` | Take the longest arc. |
| `ANIM_ROTATION_CW` | Force clockwise. |
| `ANIM_ROTATION_CCW` | Force counter-clockwise. |
| `ANIM_ROTATION_DIRECT` | Lerp angle values directly, no wrap. |

### `UI.ANIM_VAR_*` (per-iteration variation modes — `iam_variation_mode`)

| Constant | Effect |
| --- | --- |
| `ANIM_VAR_NONE` | No variation. |
| `ANIM_VAR_INCREMENT` | `+amount` each iteration. |
| `ANIM_VAR_DECREMENT` | `-amount` each iteration. |
| `ANIM_VAR_MULTIPLY` | `*amount` each iteration. |
| `ANIM_VAR_RANDOM` | Uniform in `[-amount, +amount]`. |
| `ANIM_VAR_RANDOM_ABS` | Uniform in `[0, amount]`. |
| `ANIM_VAR_PINGPONG` | Alternate `+/-` each iteration. |
| `ANIM_VAR_CALLBACK` | Defer to a registered function. **Not bound** — see deferred-callback TODO at top of `src/natives/ui/imanim.cpp`. Constants exposed for completeness only. |

### `UI.ANIM_DIR_*` (clip playback direction — `iam_direction`)

`NORMAL`, `REVERSE`, `ALTERNATE`.

### `UI.ANIM_OK` / `UI.ANIM_ERR_*` (clip persistence result codes — `iam_result`)

| Constant | Meaning |
| --- | --- |
| `ANIM_OK` | Success (`0`). |
| `ANIM_ERR_NOT_FOUND` | Clip ID / file not found. |
| `ANIM_ERR_BAD_ARG` | Invalid argument. |
| `ANIM_ERR_NO_MEM` | Allocation failed. |
