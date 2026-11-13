#ifndef BEDROCKMAP_RESOURCEMANAGER_H
#define BEDROCKMAP_RESOURCEMANAGER_H

#include <QImage>
#include <QColor>
#include <mutex>
#include <optional>
#include <string_view>
#include <QString>
#include <map>
#include <memory>
#include <vector>

class AddonResources;

namespace setting {
    struct Settings;
}

/// Owns entity images and addon presentation resources for an application or world.
class ResourceManager final {
   public:
    explicit ResourceManager(const ResourceManager* fallback = nullptr) : fallback_(fallback) {}
    ~ResourceManager() = default;
    ResourceManager(const ResourceManager&) = delete;
    ResourceManager& operator=(const ResourceManager&) = delete;

    void initialize();

    [[nodiscard]] const QImage* actorImage(const QString& key) const;
    [[nodiscard]] QString actorLabel(const QString& identifier, const QString& customName = {}) const;
    [[nodiscard]] QString actorIconUrl(const QString& identifier) const;
    [[nodiscard]] std::optional<QColor> addonBlockColor(std::string_view identifier) const;
    void setAddonResources(std::shared_ptr<const AddonResources> resources);
    [[nodiscard]] std::shared_ptr<const ResourceManager> snapshot() const;


    /// Entity images and the unknown fallback, packed by the sprite overlay.
    [[nodiscard]] std::vector<const QImage*> markerIcons() const;

   private:
    const ResourceManager* fallback_{nullptr};
    mutable std::mutex addon_mutex_;
    std::shared_ptr<const AddonResources> addons_;
    std::shared_ptr<const ResourceManager> fallback_owner_;
    const ResourceManager* bundled_images_{nullptr};
    std::map<QString, std::unique_ptr<QImage>> actor_images_;
    std::unique_ptr<QImage> fallback_image_;
};

/// Registers bundled fonts and applies the active UI font size.
class FontManager final {
   public:
    FontManager() = default;
    ~FontManager() = default;
    FontManager(const FontManager&) = delete;
    FontManager& operator=(const FontManager&) = delete;

    void initialize(const setting::Settings& settings);
    void applySize(int point_size);

   private:
    bool initialized_{false};
};

#endif  // BEDROCKMAP_RESOURCEMANAGER_H
