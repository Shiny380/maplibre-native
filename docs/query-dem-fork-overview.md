# Raster DEM query fork overview

## Purpose

The `port-query-dem-13.6.1` branch adds **query-only access to raster DEM elevation** on top of the current MapLibre Native fork `main`.

The goal is to let app code ask "what elevation is at this lat/lng?" from a loaded `raster-dem` source without adding full terrain rendering/support.

This is **not full terrain support**. It does **not** add draped terrain, terrain mesh queries, or GL JS-equivalent terrain APIs.

## Branch structure

The branch is based directly on the current fork `main` and ports the DEM-query feature from the older `tmp/query-dem` branch.

The DEM port is split into separate commits for:

1. core renderer + Android API
2. iOS API
3. DEM regression tests
4. fork documentation + local iOS/Android build tooling

The old temporary Vulkan workaround from `tmp/query-dem` is intentionally **not** included.

## High-level design

The feature is implemented in the **core renderer**, then exposed through the **Android** and **iOS** map APIs.

Flow:

1. app calls a platform API with `sourceId` + coordinate
2. platform bridge forwards to `Renderer::queryRasterDEMElevation(...)`
3. renderer finds the render source by id
4. source must be `raster-dem`
5. source scans **currently retained/renderable loaded DEM tiles**
6. best tile is chosen
7. elevation is sampled from DEM pixels with **bilinear interpolation**
8. value is returned in **meters**, **without terrain exaggeration**

No extra tile loading happens during a query.

## Core behavior

Main files:

- `include/mln/renderer/renderer.hpp`
- `src/mln/renderer/renderer.cpp`
- `src/mln/renderer/render_orchestrator.hpp`
- `src/mln/renderer/render_orchestrator.cpp`
- `src/mln/renderer/sources/render_raster_dem_source.hpp`
- `src/mln/renderer/sources/render_raster_dem_source.cpp`

Core API:

```cpp
std::optional<double> queryRasterDEMElevation(
    const std::string& sourceID,
    const LatLng& latLng
) const;
```

Important query rules:

- returns `nullopt` if the source is missing
- returns `nullopt` if the source exists but is not `raster-dem`
- returns `nullopt` if the source is not currently enabled for rendering
- returns `nullopt` if loaded/renderable DEM tile coverage is unavailable
- returns `nullopt` if the tile exists but DEM bucket/data is not ready
- returns raw DEM elevation in meters
- does not apply terrain exaggeration
- does not trigger fetch/load work

### Tile selection

Implementation lives in `RenderRasterDEMSource::queryElevation(...)`.

The implementation:

- checks `isEnabled()`
- scans the current `tilePyramid`
- only considers tiles where:
  - the tile exists
  - tile kind is `RasterDEM`
  - tile `isRenderable()`
  - a bucket exists and `hasData()`
  - the queried lat/lng falls inside the tile
- if multiple tiles match:
  - prefers **higher canonical zoom**
  - then prefers **smaller wrap magnitude**

### Coordinate conversion

The old implementation used `TileCoordinate::fromLatLng(...)`, which no longer exists on current `main`.

The port uses the equivalent current projection path:

```cpp
const double scale = std::pow(2.0, tileID.canonical.z);
const auto projected = Projection::project(latLng, scale);
const double tileX = projected.x / util::tileSize_D;
const double tileY = projected.y / util::tileSize_D;
```

Wrapped tile offsets are then applied in the same fractional tile-coordinate space.

### Sampling

Sampling is bilinear, not nearest-neighbor.

It:

- converts lat/lng to tile-local coordinates
- converts the local position to DEM pixel space
- samples four DEM values
- bilinearly interpolates them
- preserves existing DEM border semantics at tile edges

Existing DEM neighbor backfill in `onTileChanged(...)` is therefore used for edge/corner queries.

### Wrapped world support

`coversLatLng(...)` explicitly accounts for wrapped tile ids, so queries support unwrapped longitudes / wrapped worlds.

## Android API

Main files:

- `platform/android/MapLibreAndroid/src/main/java/org/maplibre/android/maps/MapLibreMap.java`
- `platform/android/MapLibreAndroid/src/main/java/org/maplibre/android/maps/NativeMap.java`
- `platform/android/MapLibreAndroid/src/main/java/org/maplibre/android/maps/NativeMapView.java`
- `platform/android/MapLibreAndroid/src/cpp/native_map_view.hpp`
- `platform/android/MapLibreAndroid/src/cpp/native_map_view.cpp`
- `platform/android/MapLibreAndroid/src/cpp/android_renderer_frontend.hpp`
- `platform/android/MapLibreAndroid/src/cpp/android_renderer_frontend.cpp`

Public API:

```java
@Nullable
public Double queryRasterDEMElevation(
    @NonNull String sourceId,
    @NonNull LatLng latLng
)
```

Android wiring:

- public `MapLibreMap` API
- `NativeMap` interface
- `NativeMapView` lifecycle/state check
- JNI method `nativeQueryRasterDEMElevation`
- `AndroidRendererFrontend`
- core renderer

The Android wrapper returns `null` after destroyed-map/lifecycle failure.

## iOS API

Main files:

- `platform/ios/src/MLNMapView.h`
- `platform/ios/src/MLNMapView.mm`

Public API:

```objc
- (nullable NSNumber *)elevationAtCoordinate:(CLLocationCoordinate2D)coordinate
           fromRasterDEMSourceWithIdentifier:(NSString *)sourceIdentifier;
```

Swift name:

```swift
elevation(at:fromRasterDEMSourceWithIdentifier:)
```

The implementation calls the core renderer directly.

The contract is the same as Android:

- queries only currently retained/loaded raster DEM tiles
- does not trigger additional loading
- does not apply terrain exaggeration
- returns `nil` for missing source, wrong type, inactive source, unavailable tiles, or uncovered coordinate

## Tests

Main files:

- `test/api/query.test.cpp`
- `test/style/source.test.cpp`

Covered behavior:

- missing source returns empty result
- wrong source type returns empty result
- bilinear interpolation
- shared tile-corner queries use DEM neighbor backfill
- higher canonical zoom is preferred
- lower zoom fallback works when higher zoom coverage is unavailable
- wrapped-world queries work
- disabled source returns empty result

Not covered:

- Android API integration tests
- iOS API integration tests
- end-to-end mobile integration tests
- platform lifecycle edge cases

## Known caveats

1. **Query-only, not terrain**
   - values come from retained raster DEM source tiles
   - values do not come from a rendered terrain mesh
   - this is not terrain API feature parity

2. **Visibility-dependent**
   - the render source must currently be enabled
   - in practice a visible layer such as hillshade needs to retain/use the DEM source

3. **Camera/load-dependent**
   - the query does not load tiles itself
   - available resolution depends on which DEM tiles are currently retained and renderable

4. **No platform integration tests**
   - core behavior has regression coverage
   - Android/iOS bridge paths still need normal platform build/runtime validation

5. **Vulkan workaround intentionally omitted**
   - old commit `11ce165eb` commented out the Vulkan framebuffer stats decrement
   - current upstream has substantial Vulkan lifecycle changes
   - the workaround should only be reconsidered if the original crash still reproduces

## Local build helpers

The branch carries local build helpers for both iOS and Android. This tooling is separate from the DEM query runtime feature.

### iOS

`scripts/build-ios-local.sh` builds the Metal iOS XCFramework with Bazel, retrieves the device dSYM, repackages the XCFramework with `xcodebuild -create-xcframework`, validates that the framework binary and dSYM UUIDs match, and installs the result into the local Swift package.

Default local package path:

```bash
scripts/build-ios-local.sh
```

Custom package path:

```bash
scripts/build-ios-local.sh --package-dir ../maplibre-ios-local
```

### Android

`scripts/build-android-local.sh` builds the Android SDK and publishes it to the local Maven repository.

Defaults:

- renderer: Vulkan
- build type: release
- ABIs: `armeabi-v7a`, `arm64-v8a`, `x86`, and `x86_64`
- Maven repository: `~/.m2/repository`
- artifact: `org.maplibre.gl:android-sdk-vulkan:13.6.1`

Default Vulkan release build:

```bash
scripts/build-android-local.sh
```

Other examples:

```bash
scripts/build-android-local.sh --renderer opengl
scripts/build-android-local.sh --renderer multiBackend
scripts/build-android-local.sh --build-type debug
scripts/build-android-local.sh --abis arm64-v8a
scripts/build-android-local.sh --abis "arm64-v8a,x86_64"
```

Renderer artifacts:

- Vulkan: `org.maplibre.gl:android-sdk-vulkan:<version>`
- OpenGL ES: `org.maplibre.gl:android-sdk-opengl:<version>`
- Vulkan + OpenGL ES: `org.maplibre.gl:android-sdk-vulkan-opengl:<version>`
- debug publications add the `-debug` artifact suffix

The script uses the existing Gradle Maven publication setup but suppresses the normal Maven Central signing configuration for local builds. It verifies that both the AAR and POM were written to the local repository.

The consuming Android project should resolve the local repository before Maven Central:

```kotlin
repositories {
    mavenLocal()
    mavenCentral()
}

dependencies {
    implementation("org.maplibre.gl:android-sdk-vulkan:13.6.1")
}
```

When rebuilding the same version repeatedly, use `--refresh-dependencies` in the consuming project if Gradle continues using a cached artifact.

## Validation status

- branch is based on current fork `main`
- DEM changes have been mechanically forward-ported to the current `mln` namespace/tree
- repository `validate-scripts` workflow has passed for the DEM commits
- a full C++/Android/iOS build has not yet been completed for this forward port

## Best files to read first

1. `src/mln/renderer/sources/render_raster_dem_source.cpp`
2. `src/mln/renderer/render_orchestrator.cpp`
3. `platform/android/MapLibreAndroid/src/main/java/org/maplibre/android/maps/MapLibreMap.java`
4. `platform/ios/src/MLNMapView.h`
5. `test/style/source.test.cpp`
6. `scripts/build-ios-local.sh`\n7. `scripts/build-android-local.sh`
