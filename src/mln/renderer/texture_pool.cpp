#include <mln/renderer/texture_pool.hpp>
#include <mln/gfx/context.hpp>
#include <mln/gfx/texture2d.hpp>
#include <mln/renderer/render_target.hpp>
#include <mln/util/logging.hpp>

namespace mln {
TexturePool::TexturePool(uint32_t tilesize)
    : tileSize(tilesize) {}

TexturePool::~TexturePool() {}

void TexturePool::createRenderTarget(gfx::Context& context, const UnwrappedTileID& id, const Color& backgroundColor) {
    // Keep an existing render target; recreating it every frame churns GL
    // texture memory and invalidates the texture bound to the terrain drawable
    if (auto it = renderTargets.find(id); it != renderTargets.end() && it->second) {
        it->second->setClearColor(backgroundColor);
        return;
    }
    // Drape targets render several overlapping draped tiles (a parent standing in for missing
    // children beside already-loaded children); the stencil clips them against each other,
    // as maplibre-gl-js does for its render-to-texture framebuffer.
    auto renderTarget = context.createRenderTarget(
        {tileSize, tileSize}, gfx::TextureChannelDataType::UnsignedByte, /*stencil=*/true);
    renderTarget->setClearColor(backgroundColor);
    renderTarget->setDrapeTileID(id);
    // The terrain samples its drape at grazing angles when pitched: mipmaps with anisotropic
    // filtering keep distant draped lines and labels sharp instead of blurred or aliased.
    if (const auto& texture = renderTarget->getTexture()) {
        texture->setSamplerConfiguration({.filter = gfx::TextureFilterType::Linear,
                                          .wrapU = gfx::TextureWrapType::Clamp,
                                          .wrapV = gfx::TextureWrapType::Clamp,
                                          .maxAnisotropy = 16,
                                          .mipmapped = true});
    }
    renderTargets[id] = std::move(renderTarget);
}

void TexturePool::removeStaleRenderTargets(const std::set<UnwrappedTileID>& currentTiles) {
    for (auto it = renderTargets.begin(); it != renderTargets.end();) {
        if (!currentTiles.contains(it->first)) {
            it = renderTargets.erase(it);
        } else {
            ++it;
        }
    }
}

std::shared_ptr<RenderTarget> TexturePool::getRenderTarget(const UnwrappedTileID& id) const {
    return renderTargets.contains(id) ? renderTargets.at(id) : nullptr;
}

std::shared_ptr<RenderTarget> TexturePool::getRenderTargetAncestorOrDescendant(
    const UnwrappedTileID& id, std::optional<UnwrappedTileID>& terrainTileID) const {
    terrainTileID = std::nullopt;
    std::shared_ptr<RenderTarget> bestRenderTarget;
    for (const auto& [tileID, renderTarget] : renderTargets) {
        if (tileID == id || tileID.isChildOf(id) || id.isChildOf(tileID)) {
            if (!terrainTileID || tileID.canonical.z > terrainTileID->canonical.z) {
                terrainTileID = tileID;
                bestRenderTarget = renderTarget;
            }
        }
    }
    return bestRenderTarget;
}
} // namespace mln
