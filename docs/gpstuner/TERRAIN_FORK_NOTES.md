# GPS Tuner 3D terrain fork – notes for catching up with upstream

State as of 2026-11-03 (NAVIE-1814, Android only; last update: draped dashed lines fix). Read this before merging a newer
upstream `feature/terrain-3d` into the fork.

## Where things are

| Repo | Branch | What |
|---|---|---|
| fork `GPS-Tuner-Systems/maplibre-native` | `update/android-v13.7.1` | fork main + tag `android-v13.7.1` (no terrain). Its AAR is on ranger `main`. |
| fork | `trial/terrain-3d` | the above + upstream `feature/terrain-3d` at `07eb06ca11e3` (merged 2026-10-30) + our fixes below. **The demo build uses this.** |
| fork | `trial/terrain-3d-v2` | attempt to merge upstream `68f204fc6fd3` (2026-11-02). Not used, see "The v2 attempt". |
| ranger `android-devs/ranger` | `trial/terrain-3d` | AAR built from fork `trial/terrain-3d`, style, side menu button |
| engine `engine-devs/engine` (`app/jni`) | `trial/terrain-3d` | elevation provider from the offline height data |
| mapviewer `android-devs/android-base` | `trial/terrain-3d` | terrain switch, camera handling, route arrows, pitch limit |

Local tags `navie-1814-cp1` … `cp8` (fork and ranger, not pushed) mark tested checkpoints.

Build the AAR: `cd platform/android && ./gradlew :MapLibreAndroid:assembleOpenglRelease`
(about 6–11 min), copy `MapLibreAndroid/build/outputs/aar/MapLibreAndroid-opengl-release.aar`
to ranger `app/libs/maplibre_android_13.7.1.aar`.

## Our changes on top of upstream terrain

"Upstream" = state of upstream `feature/terrain-3d` at `68f204fc6fd3` (2026-11-02). Re-check
each line when merging a newer upstream: if upstream fixed it, take theirs and drop ours.

### Needed by the app (our own features)

| Commit | What | Why |
|---|---|---|
| `5bce25c5a04c` | `gtdem://{z}/{x}/{y}` raster-dem tiles generated in-process by a C callback (`mln_gpst_set_dem_provider`, exported), on their own worker thread, newest first, cancel-aware | the terrain uses our offline height data (`_terrain` files) through the engine |
| `ce2989903093` | the custom MBTiles hook answers only `.pbf`/`.mvt` requests | it answered the raster-dem requests with vector tiles |
| `d4f19bd1d313` | Android `Projection.toScreenLocation/fromScreenLocation(…, elevation)`, `MapLibreMap.setTileLodMode` | pin/tap positions on the terrain; ADAPTIVE tile LOD (see below). Not in upstream. |
| `da608fe43b3f` | `Map::setCenterAltitude` / Android `MapLibreMap.setCenterAltitude`: hold the centre altitude over running transitions | close-up terrain switch drives the camera height with the growing relief |

### Bug fixes and improvements in upstream code

| Commit | Symptom it fixed | Upstream status |
|---|---|---|
| `320f8278440d` | terrain flickers away for single frames (GL): `LayerGroupGL::render` did not reset the stencil test, the 3D mesh inherited the previous draw's tile clipping test | still present |
| `7816781ed7f8` | any terrain change (even only the exaggeration) rebuilt the whole `RenderTerrain` – needed to animate the exaggeration | still present |
| `f52c0f29846b`, `7ec5f25e772b` | empty band at the horizon of pitched views: over the mesh tile cap far tiles were dropped. Coarsen further (only real merges, not below DEM minzoom); the DEM source also loads the mesh tiles | still present (upstream reworked the coarsening loop itself) |
| `965f31146485` | close-ups blurry: Balanced/Performance stopped the mesh at z14/z15, stretching drape targets 16× | still present (`meshTargetZoom` 15/14) |
| `9ad001b439b2` | speckled accuracy circle / recorded track: overlapping mesh tiles from the cover dilation z-fight | **fixed upstream** (`dilateCover`) |
| `624be39dcd58` | distant draped lines blurry/aliasing when pitched: GL backend ignored `SamplerState::mipmapped`/`maxAnisotropy`; drape targets now mipmapped + 16× anisotropic | still present |
| `604de1c23f39` | thin outlines broken into dashes: drape targets fixed at 1024 px; now 512 × pixel ratio (1024–2048) with one shared depth/stencil buffer | still present |
| `aa4d8fb5ea09` | band of blocky roads mid-view: over the tile cap merge the deepest tiles first | still present |
| `75ed1d33f468` | cursor slid off / view jerked: clamped centre altitude snapped only after transitions. Eases on every frame, also during transitions | **partly fixed upstream** (#4865, #4870, #4873 – gesture hold, eye clearance, animation target height) |
| `1051aa29a67a` | route arrows / labels missing in front of the camera over high ground: collision index projected symbols at sea level | still present |
| `548a37a39946` | dashed paths/tracks/cycleways invisible on terrain (thin ones gone, wide ones faded): SDF, pattern and gradient line shaders used the screen perspective gamma scale in the drape pass; only the simple line shader had the drape branch | still present (found 2026-11-03) |

## The app side (depends on the fork APIs above)

- Style `custom_style.json`: `gtdem` raster-dem source, `tileSize` 512 (fewer mesh tiles for the
  same view – important for the 24-tile cap), z6–12, terrain exaggeration 1.3, hillshade layer
  with `hillshade-illumination-anchor: map`.
- `SharedMapViewHost`: `TerrainLoadMode.PERFORMANCE` (QUALITY peaked at 1.3 GB GPU memory),
  `TileLodMode.ADAPTIVE` (DEFAULT requested ~45 000 tiles to the horizon and froze the app),
  `setCenterClampedToGround(true)` (cursor stays in place on slopes).
- Terrain switch (`MapTerrainHelper`, `SharedMapViewHost.setTerrainEnabled`): exaggeration
  animated 0 ↔ 1.3. Below z16 the clamp is off during the switch (camera still); from z16
  switching on drives `setCenterAltitude` every frame (0.6–1.5 s by rise/camera distance).
  Switching off keeps a flat terrain until the centre altitude is back to ~0, then removes it.
- Screen ↔ map positions on the terrain use the engine's height (`GTMapViewer.getTerrainElevation`).
- Route arrows: none on the passed route; invisible colliding icons along the maneuver arrow
  keep the arrows off it (draped lines are always under symbols with terrain).
- Pitch limit: 0° to z7, 45° at z12, 60° from z15.

Measured on Samsung S21: GPU graphics memory ~600 MB average, ~750 MB peak (Xiaomi kills apps
over 1.5 GB).

## The v2 attempt (upstream `68f204fc6fd3`, branch `trial/terrain-3d-v2`)

Merged cleanly except six files. Results on the device:
- cursor following: good (upstream's new ground clamp);
- terrain switch: the old jumps came back – our easing (`75ed1d33f468`) was dropped in favour of
  upstream's clamp, and the switch tuning depends on it;
- **new regression**: lines (dashed paths) and POI icons missing in close-ups with terrain.
  Probably upstream #4827 (per-source tile limit nearest the camera under terrain) combined with
  our deep close-up mesh; not verified.

## Checklist when catching up with a newer upstream

1. `git fetch upstream feature/terrain-3d`; read the new commits touching `render_terrain`,
   `map_impl` (centre clamp), `tile_pyramid`/`tile_cover`, `collision_index`, `texture2d`.
2. Merge into a **new** branch (keep `trial/terrain-3d`). Expect conflicts in `mode.hpp`,
   `render_terrain.cpp`, `map_impl.cpp`, `render_orchestrator.cpp`, `tile_parameters.hpp`,
   `transform.hpp`. If `vendor/` changed from submodule to in-tree, move the old submodule
   directory aside first.
3. Drop our fixes that upstream now covers (table above), keep the app-required ones.
4. Test on a device, with navigation:
   - Budapest hills and Innsbruck (pitched, rotated, z8–z20): no flicker, no empty bands,
     no blocky/blurry roads, dashed paths and POI icons present;
   - terrain switch on/off, close (z17+) and far (z12): no jumps, cursor stays in place;
   - cursor following uphill/downhill, maneuver arrow without route arrows on it;
   - GPU memory: `adb shell dumpsys meminfo com.gpstuner.navie` (Graphics, GL mtrack).
5. Diagnostics that helped: temporary `debug.*` system properties read at runtime
   (`__system_property_get`) to toggle code paths without rebuilding; per-frame mesh cover
   logging; collision boxes (`map.isDebugActive = true`).
