#include <mbgl/renderer/sources/render_raster_dem_source.hpp>
#include <mbgl/renderer/render_tile.hpp>
#include <mbgl/tile/raster_dem_tile.hpp>
#include <mbgl/algorithm/update_tile_masks.hpp>
#include <mbgl/geometry/dem_data.hpp>
#include <mbgl/renderer/buckets/hillshade_bucket.hpp>
#include <mbgl/renderer/tile_parameters.hpp>
#include <mbgl/util/tile_coordinate.hpp>

#include <cmath>

namespace mbgl {

using namespace style;

namespace {

struct ElevationCandidate {
    const RasterDEMTile* tile;
    const DEMData* dem;
    double localX;
    double localY;
};

bool coversLatLng(const OverscaledTileID& tileID, const LatLng& latLng, double& localX, double& localY) {
    const auto coordinate = TileCoordinate::fromLatLng(tileID.canonical.z, latLng);
    const double worldSize = std::pow(2.0, tileID.canonical.z);
    // `TileCoordinate::fromLatLng` preserves unwrapped longitudes, so wrapped tiles live at
    // canonical x offset by `wrap * worldSize` in that same coordinate space.
    const double x = coordinate.p.x - static_cast<double>(tileID.wrap) * worldSize;

    localX = x - tileID.canonical.x;
    localY = coordinate.p.y - tileID.canonical.y;

    return localX >= 0.0 && localX <= 1.0 && localY >= 0.0 && localY <= 1.0;
}

double sampleElevation(const DEMData& dem, const double localX, const double localY) {
    const double px = localX * dem.dim - 0.5;
    const double py = localY * dem.dim - 0.5;

    const auto x0 = static_cast<int32_t>(std::floor(px));
    const auto x1 = x0 + 1;
    const auto y0 = static_cast<int32_t>(std::floor(py));
    const auto y1 = y0 + 1;

    const double tx = px - x0;
    const double ty = py - y0;

    // Preserve DEM border semantics at tile edges. `DEMData` allows taps in [-1, dim].
    const double top = dem.get(x0, y0) * (1.0 - tx) + dem.get(x1, y0) * tx;
    const double bottom = dem.get(x0, y1) * (1.0 - tx) + dem.get(x1, y1) * tx;
    return top * (1.0 - ty) + bottom * ty;
}

bool isBetterCandidate(const ElevationCandidate& candidate, const ElevationCandidate& best) {
    if (candidate.tile->id.canonical.z != best.tile->id.canonical.z) {
        return candidate.tile->id.canonical.z > best.tile->id.canonical.z;
    }

    return std::abs(candidate.tile->id.wrap) < std::abs(best.tile->id.wrap);
}

} // namespace

RenderRasterDEMSource::RenderRasterDEMSource(Immutable<style::TileSource::Impl> impl_,
                                             const TaggedScheduler& threadPool_)
    : RenderTileSetSource(std::move(impl_), threadPool_) {}

const style::TileSource::Impl& RenderRasterDEMSource::impl() const {
    return static_cast<const style::TileSource::Impl&>(*baseImpl);
}

const std::optional<Tileset>& RenderRasterDEMSource::getTileset() const {
    return impl().tileset;
}

std::optional<double> RenderRasterDEMSource::queryElevation(const LatLng& latLng) const {
    if (!isEnabled() || !getTileset()) {
        return std::nullopt;
    }

    const auto& tiles = tilePyramid.getTiles();
    if (tiles.empty()) {
        return std::nullopt;
    }

    std::optional<ElevationCandidate> best;

    for (const auto& [_, tile] : tiles) {
        if (!tile || tile->kind != Tile::Kind::RasterDEM || !tile->isRenderable()) {
            continue;
        }

        const auto* demTile = static_cast<const RasterDEMTile*>(tile.get());
        const auto* bucket = demTile->getBucket();
        if (!bucket || !bucket->hasData()) {
            continue;
        }

        double localX = 0.0;
        double localY = 0.0;
        if (!coversLatLng(demTile->id, latLng, localX, localY)) {
            continue;
        }

        ElevationCandidate candidate{demTile, &bucket->getDEMData(), localX, localY};
        // Canonical zoom tracks source DEM resolution. Overscaled zoom does not.
        if (!best || isBetterCandidate(candidate, *best)) {
            best = candidate;
        }
    }

    if (!best) {
        return std::nullopt;
    }

    return sampleElevation(*best->dem, best->localX, best->localY);
}

void RenderRasterDEMSource::updateInternal(const Tileset& tileset,
                                           const std::vector<Immutable<LayerProperties>>& layers,
                                           const bool needsRendering,
                                           const bool needsRelayout,
                                           const TileParameters& parameters) {
    tilePyramid.update(layers,
                       needsRendering,
                       needsRelayout,
                       parameters,
                       *baseImpl,
                       impl().getTileSize(),
                       tileset.zoomRange,
                       tileset.bounds,
                       [&](const OverscaledTileID& tileID, TileObserver* observer_) {
                           return std::make_unique<RasterDEMTile>(tileID, baseImpl->id, parameters, tileset, observer_);
                       });
    algorithm::updateTileMasks(tilePyramid.getRenderedTiles());
}

void RenderRasterDEMSource::onTileChanged(Tile& tile) {
    auto& demtile = static_cast<RasterDEMTile&>(tile);

    std::map<DEMTileNeighbors, DEMTileNeighbors> opposites = {
        {DEMTileNeighbors::Left, DEMTileNeighbors::Right},
        {DEMTileNeighbors::Right, DEMTileNeighbors::Left},
        {DEMTileNeighbors::TopLeft, DEMTileNeighbors::BottomRight},
        {DEMTileNeighbors::TopCenter, DEMTileNeighbors::BottomCenter},
        {DEMTileNeighbors::TopRight, DEMTileNeighbors::BottomLeft},
        {DEMTileNeighbors::BottomRight, DEMTileNeighbors::TopLeft},
        {DEMTileNeighbors::BottomCenter, DEMTileNeighbors::TopCenter},
        {DEMTileNeighbors::BottomLeft, DEMTileNeighbors::TopRight}};

    if (tile.isRenderable() && demtile.neighboringTiles != DEMTileNeighbors::Complete) {
        const CanonicalTileID canonical = tile.id.canonical;
        const auto dim = static_cast<uint32_t>(std::pow(2, canonical.z));
        const uint32_t px = (canonical.x - 1 + dim) % dim;
        const int pxw = canonical.x == 0 ? tile.id.wrap - 1 : tile.id.wrap;
        const uint32_t nx = (canonical.x + 1 + dim) % dim;
        const int nxw = (canonical.x + 1 == dim) ? tile.id.wrap + 1 : tile.id.wrap;

        auto getNeighbor = [&](DEMTileNeighbors mask) {
            if (mask == DEMTileNeighbors::Left) {
                return OverscaledTileID(tile.id.overscaledZ, pxw, canonical.z, px, canonical.y);
            } else if (mask == DEMTileNeighbors::Right) {
                return OverscaledTileID(tile.id.overscaledZ, nxw, canonical.z, nx, canonical.y);
            } else if (mask == DEMTileNeighbors::TopLeft) {
                return OverscaledTileID(tile.id.overscaledZ, pxw, canonical.z, px, canonical.y - 1);
            } else if (mask == DEMTileNeighbors::TopCenter) {
                return OverscaledTileID(tile.id.overscaledZ, tile.id.wrap, canonical.z, canonical.x, canonical.y - 1);
            } else if (mask == DEMTileNeighbors::TopRight) {
                return OverscaledTileID(tile.id.overscaledZ, nxw, canonical.z, nx, canonical.y - 1);
            } else if (mask == DEMTileNeighbors::BottomLeft) {
                return OverscaledTileID(tile.id.overscaledZ, pxw, canonical.z, px, canonical.y + 1);
            } else if (mask == DEMTileNeighbors::BottomCenter) {
                return OverscaledTileID(tile.id.overscaledZ, tile.id.wrap, canonical.z, canonical.x, canonical.y + 1);
            } else if (mask == DEMTileNeighbors::BottomRight) {
                return OverscaledTileID(tile.id.overscaledZ, nxw, canonical.z, nx, canonical.y + 1);
            } else {
                throw std::runtime_error("mask is not a valid tile neighbor");
            }
        };

        for (uint8_t i = 0; i < 8; i++) {
            auto mask = DEMTileNeighbors(std::pow(2, i));
            // only backfill if this neighbor has not been previously backfilled
            if ((demtile.neighboringTiles & mask) != mask) {
                OverscaledTileID neighborid = getNeighbor(mask);
                Tile* renderableNeighbor = tilePyramid.getTile(neighborid);
                if (renderableNeighbor != nullptr && renderableNeighbor->isRenderable()) {
                    auto& borderTile = static_cast<RasterDEMTile&>(*renderableNeighbor);
                    demtile.backfillBorder(borderTile, mask);

                    // if the border tile has not been backfilled by a previous
                    // instance of the main tile, backfill its corresponding
                    // neighbor as well.
                    const DEMTileNeighbors& borderMask = opposites[mask];
                    if ((borderTile.neighboringTiles & borderMask) != borderMask) {
                        borderTile.backfillBorder(demtile, borderMask);
                    }
                }
            }
        }
    }
    RenderTileSource::onTileChanged(tile);
}

std::unordered_map<std::string, std::vector<Feature>> RenderRasterDEMSource::queryRenderedFeatures(
    const ScreenLineString&,
    const TransformState&,
    const std::unordered_map<std::string, const RenderLayer*>&,
    const RenderedQueryOptions&,
    const mat4&) const {
    return std::unordered_map<std::string, std::vector<Feature>>{};
}

std::vector<Feature> RenderRasterDEMSource::querySourceFeatures(const SourceQueryOptions&) const {
    return {};
}

} // namespace mbgl
