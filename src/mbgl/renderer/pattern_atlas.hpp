#pragma once

#include <mapbox/shelf-pack.hpp>
#include <mbgl/gfx/texture.hpp>
#include <mbgl/renderer/image_atlas.hpp>
#include <mbgl/style/image_impl.hpp>

#include <unordered_map>
#include <string>
#include <optional>

namespace mbgl {

template <class T>
class Actor;

namespace gfx {
class Texture2D;
class UploadPass;
} // namespace gfx

class PatternAtlas {
public:
    PatternAtlas();
    PatternAtlas(const PatternAtlas&) = delete;
    PatternAtlas& operator=(const PatternAtlas&) = delete;
    ~PatternAtlas();

    std::optional<ImagePosition> getPattern(const std::string&) const;
    std::optional<ImagePosition> addPattern(const style::Image::Impl&);
    void removePattern(const std::string&);

    const std::shared_ptr<gfx::Texture2D>& texture() const;

    void upload(gfx::UploadPass&);
    Size getPixelSize() const;

    const PremultipliedImage& getAtlasImageForTests() const { return atlasImage; }

    bool isEmpty() const { return patterns.empty(); }

private:
    struct Pattern {
        mapbox::Bin* bin;
        ImagePosition position;
    };
    mapbox::ShelfPack shelfPack;
    std::unordered_map<std::string, Pattern> patterns;
    PremultipliedImage atlasImage;
    std::shared_ptr<gfx::Texture2D> atlasTexture2D{nullptr};
    bool dirty = true;
};

} // namespace mbgl
