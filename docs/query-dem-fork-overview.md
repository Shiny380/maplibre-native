# `tmp/query-dem` fork overview

## Purpose

This branch adds temporary **query-only access to raster DEM elevation** in MapLibre Native.

Goal: let app code ask "what elevation is at this lat/lng?" from a loaded `raster-dem` source, without adding full terrain rendering/support like MapLibre GL JS.

This is **not full terrain support**. It does **not** add draped terrain, terrain mesh queries, or JS-equivalent terrain APIs.

## Branch delta vs upstream

Original upstream repo: `https://github.com/maplibre/maplibre-native.git`

Branch `tmp/query-dem` is upstream snapshot + **2 fork-only commits**. It is also behind current upstream `main` by 121 commits, so review/rebase work should assume upstream drift.


1. `6175ccd2b` — `temporary query raster-dem elevation`
2. `11ce165eb` — `tmp prevent vulkan crash`

`tmp/query-dem` is where fork-specific behavior lives.

## High-level design

Feature is implemented in **core renderer**, then exposed through **Android** and **iOS** map APIs.

Flow:

1. app calls platform API with `sourceId` + coordinate
2. platform bridge forwards to core `Renderer::queryRasterDEMElevation(...)`
3. renderer finds render source by id
4. source must be `raster-dem`
5. source scans **currently retained/renderable loaded DEM tiles**
6. best tile is chosen
7. elevation sampled from DEM pixels with **bilinear interpolation**
8. value returned in **meters**, **without terrain exaggeration**

No extra tile loading happens during query.

## Core behavior

Main files:

- `include/mbgl/renderer/renderer.hpp`
- `src/mbgl/renderer/renderer.cpp`
- `src/mbgl/renderer/render_orchestrator.hpp`
- `src/mbgl/renderer/render_orchestrator.cpp`
- `src/mbgl/renderer/sources/render_raster_dem_source.hpp`
- `src/mbgl/renderer/sources/render_raster_dem_source.cpp`

Core API added:

```cpp
std::optional<double> queryRasterDEMElevation(const std::string& sourceID, const LatLng& latLng) const;
```

Important query rules:

- returns `nullopt` if source missing
- returns `nullopt` if source exists but is not `raster-dem`
- returns `nullopt` if source is not currently enabled for rendering
- returns `nullopt` if loaded/renderable DEM tile coverage is unavailable
- returns `nullopt` if tile exists but DEM bucket/data is not ready
- returns raw DEM elevation in meters
- does not apply terrain exaggeration
- does not trigger fetch/load work

### Tile selection details

Implementation lives in `RenderRasterDEMSource::queryElevation(...)`.

Key details:

- checks `isEnabled()` first
  - in practice this means source must be retained by visible rendering work
  - usually: style needs visible layer using source, e.g. hillshade layer
- scans current `tilePyramid`
- only considers tiles where:
  - tile exists
  - tile kind is `RasterDEM`
  - tile `isRenderable()`
  - bucket exists and `hasData()`
  - queried lat/lng falls inside tile
- if multiple tiles match:
  - prefers **higher canonical zoom**
  - then prefers **smaller wrap magnitude**

### Sampling details

Sampling is not nearest-neighbor.

It does:

- convert lat/lng to tile-local coordinates
- convert local position to DEM pixel space
- bilinear interpolate 4 DEM samples
- preserve DEM border semantics at tile edges

Edge/corner correctness is helped by existing DEM neighbor backfill logic in `onTileChanged(...)`.

### Wrapped world support

`coversLatLng(...)` explicitly adjusts for wrapped tile ids. Query code supports unwrapped longitudes / wrapped worlds.

## Android surface

Main files:

- `platform/android/MapLibreAndroid/src/main/java/org/maplibre/android/maps/MapLibreMap.java`
- `platform/android/MapLibreAndroid/src/main/java/org/maplibre/android/maps/NativeMap.java`
- `platform/android/MapLibreAndroid/src/main/java/org/maplibre/android/maps/NativeMapView.java`
- `platform/android/MapLibreAndroid/src/cpp/native_map_view.hpp`
- `platform/android/MapLibreAndroid/src/cpp/native_map_view.cpp`
- `platform/android/MapLibreAndroid/src/cpp/android_renderer_frontend.hpp`
- `platform/android/MapLibreAndroid/src/cpp/android_renderer_frontend.cpp`

Public Android API:

```java
@Nullable
public Double queryRasterDEMElevation(@NonNull String sourceId, @NonNull LatLng latLng)
```

Android wiring is complete:

- Java API on `MapLibreMap`
- `NativeMap` interface
- `NativeMapView` lifecycle/state check
- JNI method `nativeQueryRasterDEMElevation`
- `AndroidRendererFrontend`
- core renderer

Android behavior notes:

- returns `null` after destroyed-map/lifecycle failure
- inline Java docs clearly describe contract
- no demo/test-app usage of this API found in repo

## iOS surface

Main files:

- `platform/ios/src/MLNMapView.h`
- `platform/ios/src/MLNMapView.mm`

Public iOS API:

```objc
- (nullable NSNumber *)elevationAtCoordinate:(CLLocationCoordinate2D)coordinate
           fromRasterDEMSourceWithIdentifier:(NSString *)sourceIdentifier;
```

Swift name:

```swift
elevation(at:fromRasterDEMSourceWithIdentifier:)
```

iOS implementation exists and directly calls core renderer. No extra bridge layer appears missing.

iOS contract docs say:

- queries only currently retained/loaded raster DEM tiles
- does not trigger additional loading
- does not apply terrain exaggeration
- returns `nil` for missing source / wrong type / inactive source / unavailable tiles / uncovered coordinate

## Tests added

Main files:

- `test/api/query.test.cpp`
- `test/style/source.test.cpp`

Covered behaviors:

- missing source returns empty result
- wrong source type returns empty result
- bilinear interpolation works
- shared tile-corner queries use neighbor backfill correctly
- higher canonical zoom is preferred when available
- lower zoom fallback works when higher zoom coverage is incomplete
- wrapped-world queries work
- disabled source returns empty result

What is **not** covered:

- Android API tests
- iOS API tests
- end-to-end mobile integration tests
- lifecycle edge cases on iOS

## Known caveats

1. **Query-only hack, not terrain system**
   - value comes from loaded DEM source tiles
   - not from rendered terrain mesh
   - not feature-parity with GL JS terrain APIs

2. **Visibility-dependent**
   - source must be enabled by current render state
   - if no visible layer keeps source active, query returns empty
   - practical setup likely needs visible hillshade or similar source consumer

3. **Camera/load dependent**
   - result quality depends on which DEM tiles are already retained
   - different zoom/camera states can change which tile is sampled

4. **No platform docs beyond inline comments**
   - no broader docs or examples found

5. **iOS untested**
   - code path exists
   - no iOS tests or sample usage found
   - implementation appears complete, but confidence lower than Android

6. **iOS lifecycle risk**
   - Android explicitly guards destroyed state and returns `null`
   - iOS method calls renderer directly; no equivalent explicit guard at this callsite
   - likely fine during normal map lifetime, but teardown behavior is less proven

## Vulkan workaround commit

File:

- `src/mbgl/vulkan/offscreen_texture.cpp`

Commit `11ce165eb` comments out:

```cpp
backend.getContext().renderingStats().numFrameBuffers--;
```

Destructor still resets framebuffer/render pass/texture resources. Change only skips decrementing Vulkan rendering stats counter.

Likely intent:

- avoid teardown-time Vulkan crash
- probably Android/Vulkan-specific workaround
- temporary hack, not proper fix

Side effect:

- `numFrameBuffers` stats can become inaccurate / leak upward

## Fast mental model for other agents

If agent needs understand this fork quickly, remember:

- branch adds **temporary DEM elevation query**, not terrain
- core logic sits in `RenderRasterDEMSource::queryElevation(...)`
- platform APIs are thin wrappers around core
- Android path looks complete and was reportedly validated
- iOS path exists but appears untested
- second commit is separate Vulkan crash workaround

## Best files to read first

1. `src/mbgl/renderer/sources/render_raster_dem_source.cpp`
2. `src/mbgl/renderer/render_orchestrator.cpp`
3. `platform/android/MapLibreAndroid/src/main/java/org/maplibre/android/maps/MapLibreMap.java`
4. `platform/ios/src/MLNMapView.h`
5. `test/style/source.test.cpp`
6. `src/mbgl/vulkan/offscreen_texture.cpp`
