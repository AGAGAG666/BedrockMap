#include "base/addonresources.h"

#include <QtCore/private/qzipreader_p.h>

#include <QBuffer>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocale>
#include <QPainter>
#include <QSet>
#include <algorithm>

#include "base/config.h"
#include "io/documentio.h"

namespace {
    QByteArray jsonClean(const QByteArray& source) {
        QByteArray out;
        bool string = false, escape = false, line = false, block = false;
        for (qsizetype i = 0; i < source.size(); ++i) {
            const char c = source[i], next = i + 1 < source.size() ? source[i + 1] : '\0';
            if (line) {
                if (c == '\n' || c == '\r') {
                    line = false;
                    out += c;
                }
                continue;
            }
            if (block) {
                if (c == '*' && next == '/') {
                    block = false;
                    ++i;
                    out += ' ';
                } else if (c == '\n')
                    out += c;
                continue;
            }
            if (!string && c == '/' && next == '/') {
                line = true;
                ++i;
                out += ' ';
                continue;
            }
            if (!string && c == '/' && next == '*') {
                block = true;
                ++i;
                out += ' ';
                continue;
            }
            if (c == '"' && !escape) string = !string;
            out += c;
            if (string && c == '\\' && !escape)
                escape = true;
            else
                escape = false;
        }
        QByteArray result;
        string = false;
        escape = false;
        for (qsizetype i = 0; i < out.size(); ++i) {
            const char c = out[i];
            if (!string && c == ',') {
                qsizetype j = i + 1;
                while (j < out.size() && (out[j] == ' ' || out[j] == '\n' || out[j] == '\r' || out[j] == '\t')) ++j;
                if (j < out.size() && (out[j] == '}' || out[j] == ']')) continue;
            }
            if (c == '"' && !escape) string = !string;
            result += c;
            if (string && c == '\\' && !escape)
                escape = true;
            else
                escape = false;
        }
        return result;
    }
    QJsonDocument json(const QByteArray& data, QStringList* warnings = nullptr, const QString& label = {}) {
        if (data.isEmpty()) return {};
        QJsonParseError error;
        const auto document = QJsonDocument::fromJson(jsonClean(data), &error);
        if (warnings && error.error != QJsonParseError::NoError)
            warnings->append(AddonResources::tr("Invalid JSON in %1: %2").arg(label, error.errorString()));
        return document;
    }
    QUrl child(const QString& root, const QString& path) { return QUrl::fromLocalFile(QDir(root).filePath(path)); }
    QByteArray read(const QUrl& url) {
        const auto r = io::DocumentIo::readAll(url);
        return r.ok() ? r.data : QByteArray{};
    }
    struct Pack {
        QString root;
        QHash<QString, QByteArray> files;
        QStringList paths;
        QString uuid;
        QJsonArray version;
        QString type;
        QByteArray get(QString path) const {
            path = QDir::cleanPath(path.replace('\\', '/'));
            if (QDir::isAbsolutePath(path) || path == ".." || path.startsWith("../")) return {};
            if (!root.isEmpty() && !paths.isEmpty() && !paths.contains(path)) return {};
            return root.isEmpty() ? files.value(path) : read(child(root, path));
        }
    };
    void scanDir(const QString& root, const QString& relative, QStringList& paths) {
        const auto listed = io::DocumentIo::list(child(root, relative));
        if (!listed.ok()) return;
        for (const auto& name : listed.names) {
            if (name == "." || name == ".." || name.contains('/') || name.contains('\\')) continue;
            const QString path = relative.isEmpty() ? name : relative + '/' + name;
            const auto status = io::DocumentIo::stat(child(root, path));
            if (!status.ok() || status.info.isLink) continue;
            if (status.info.isDirectory)
                scanDir(root, path, paths);
            else if (status.info.isFile)
                paths.append(path);
        }
    }
    bool manifest(Pack& p, QStringList* warnings = nullptr) {
        const auto obj = json(p.get("manifest.json"), warnings, p.root + "/manifest.json").object();
        const auto header = obj.value("header").toObject();
        p.uuid = header.value("uuid").toString().toLower();
        p.version = header.value("version").toArray();
        for (const auto& module : obj.value("modules").toArray()) {
            const auto type = module.toObject().value("type").toString();
            if (type == "resources")
                p.type = "resources";
            else if (type == "data" && p.type.isEmpty())
                p.type = "data";
        }
        return !p.uuid.isEmpty() && !p.type.isEmpty();
    }
    void zipPacks(const QByteArray& data, QList<Pack>& packs, QStringList& warnings, const QString& label) {
        QBuffer buffer;
        buffer.setData(data);
        buffer.open(QIODevice::ReadOnly);
        QZipReader zip(&buffer);
        const auto entries = zip.fileInfoList();
        if (zip.status() != QZipReader::NoError || entries.isEmpty()) {
            warnings << AddonResources::tr("Invalid pack archive: %1").arg(label);
            return;
        }
        QHash<QString, QByteArray> files;
        QStringList nested;
        for (const auto& entry : entries) {
            if (!entry.isFile) continue;
            const QString path = QDir::cleanPath(entry.filePath);
            const QString suffix = QFileInfo(path).suffix().toLower();
            if (suffix != "json" && suffix != "lang" && suffix != "png" && suffix != "tga" && suffix != "mcpack") continue;
            if (path.startsWith('/') || path == ".." || path.startsWith("../")) continue;
            if (path.endsWith(".mcpack", Qt::CaseInsensitive)) nested << path;
            files.insert(path, zip.fileData(entry.filePath));
        }
        if (zip.status() != QZipReader::NoError) {
            warnings << AddonResources::tr("Unreadable pack archive: %1").arg(label);
            return;
        }
        QStringList roots;
        for (auto it = files.cbegin(); it != files.cend(); ++it)
            if (it.key() == "manifest.json" || it.key().endsWith("/manifest.json")) roots << it.key().left(it.key().size() - 13);
        roots.sort();
        for (const auto& root : roots) {
            Pack pack;
            for (auto it = files.cbegin(); it != files.cend(); ++it)
                if (it.key().startsWith(root)) {
                    const auto name = it.key().mid(root.size());
                    pack.files.insert(name, it.value());
                    pack.paths << name;
                }
            if (manifest(pack, &warnings)) packs << std::move(pack);
        }
        for (const auto& path : nested) zipPacks(files.value(path), packs, warnings, label + ":" + path);
    }
    void addSource(const QUrl& source, QList<Pack>& packs, QStringList& warnings) {
        const auto status = io::DocumentIo::stat(source);
        if (status.ok() && status.info.isDirectory) {
            Pack p;
            p.root = io::DocumentIo::resolveNativeDirectory(source);
            if (p.root.isEmpty()) {
                warnings << AddonResources::tr("Pack directory is unavailable: %1").arg(io::DocumentIo::displayName(source));
                return;
            }
            scanDir(p.root, {}, p.paths);
            if (manifest(p, &warnings))
                packs << std::move(p);
            else
                warnings << AddonResources::tr("Invalid pack manifest: %1").arg(io::DocumentIo::displayName(source));
            return;
        }
        const auto result = io::DocumentIo::readAll(source);
        if (!result.ok()) {
            warnings << AddonResources::tr("Cannot read pack: %1").arg(io::DocumentIo::displayName(source));
            return;
        }
        zipPacks(result.data, packs, warnings, io::DocumentIo::displayName(source));
    }
    QHash<QString, QString> lang(const Pack& pack, const QString& language) {
        QHash<QString, QString> out;
        const auto lines = QString::fromUtf8(pack.get("texts/" + language + ".lang")).split('\n');
        for (QString line : lines) {
            const auto comment = line.indexOf("##");
            if (comment >= 0) line.truncate(comment);
            const auto equals = line.indexOf('=');
            if (equals <= 0) continue;
            out.insert(line.left(equals).trimmed(), line.mid(equals + 1).trimmed());
        }
        return out;
    }
    QString texturePath(const QJsonValue& value) {
        if (value.isString()) return value.toString();
        if (value.isArray()) {
            for (const auto& v : value.toArray()) {
                const auto p = texturePath(v);
                if (!p.isEmpty()) return p;
            }
        }
        if (value.isObject()) {
            const auto o = value.toObject();
            if (o.contains("path")) return texturePath(o.value("path"));
            if (o.contains("textures")) return texturePath(o.value("textures"));
        }
        return {};
    }
    QImage image(const Pack& pack, QString path) {
        if (path.isEmpty()) return {};
        QImage result;
        result.loadFromData(pack.get(path));
        if (result.isNull() && !path.endsWith(".png", Qt::CaseInsensitive)) result.loadFromData(pack.get(path + ".png"));
        if (result.isNull() && !path.endsWith(".tga", Qt::CaseInsensitive)) result.loadFromData(pack.get(path + ".tga"));
        return result;
    }
    QColor average(const QImage& source, bool blend = false) {
        if (source.isNull()) return {};
        const QImage img = source.convertToFormat(QImage::Format_ARGB32);
        quint64 r = 0, g = 0, b = 0, weight = 0;
        for (int y = 0; y < img.height(); ++y) {
            const auto* row = reinterpret_cast<const QRgb*>(img.constScanLine(y));
            for (int x = 0; x < img.width(); ++x) {
                const auto pixel = row[x];
                const int a = qAlpha(pixel);
                weight += a;
                r += qRed(pixel) * a;
                g += qGreen(pixel) * a;
                b += qBlue(pixel) * a;
            }
        }
        if (!weight) return QColor(Qt::transparent);
        const int alpha = blend ? int(weight / (quint64(img.width()) * img.height())) : 255;
        return QColor(int(r / weight), int(g / weight), int(b / weight), alpha);
    }
    QHash<QString, QJsonValue> textureTable(const Pack& p, const QString& file, QStringList* warnings) {
        QHash<QString, QJsonValue> result;
        const auto entries = json(p.get(file), warnings, file).object().value("texture_data").toObject();
        for (auto i = entries.begin(); i != entries.end(); ++i) result.insert(i.key(), i.value());
        return result;
    }
    QString textureReference(const QJsonValue& value) {
        if (value.isString()) return value.toString();
        if (value.isObject()) {
            const auto object = value.toObject();
            if (object.contains("texture")) return textureReference(object.value("texture"));
            for (const auto& key : {QStringLiteral("up"), QStringLiteral("*"), QStringLiteral("side"), QStringLiteral("north")})
                if (object.contains(key)) {
                    const auto found = textureReference(object.value(key));
                    if (!found.isEmpty()) return found;
                }
            for (auto i = object.begin(); i != object.end(); ++i) {
                const auto found = textureReference(i.value());
                if (!found.isEmpty()) return found;
            }
        }
        return {};
    }
    bool blendedMaterial(const QJsonValue& value) {
        const auto object = value.toObject();
        if (object.value("render_method").toString() == "blend") return true;
        for (auto it = object.begin(); it != object.end(); ++it)
            if (it.value().isObject() && blendedMaterial(it.value())) return true;
        return false;
    }
    struct LocatedTexture {
        const Pack* pack = nullptr;
        QString path;
    };
    QImage locateImage(const QHash<QString, LocatedTexture>& table, const QString& name, const QHash<QString, LocatedTexture>& files) {
        const auto entry = table.value(name);
        const auto replacement = files.value(entry.path);
        if (replacement.pack) return image(*replacement.pack, replacement.path);
        return entry.pack ? image(*entry.pack, entry.path) : QImage{};
    }
    QString indexedTexture(QJsonValue value, int index) {
        if (value.isObject() && value.toObject().contains("textures")) value = value.toObject().value("textures");
        if (value.isArray()) {
            const auto variants = value.toArray();
            return index >= 0 && index < variants.size() ? texturePath(variants[index]) : QString{};
        }
        return index == 0 ? texturePath(value) : QString{};
    }
    QImage cubeIcon(const QImage& skin, const QJsonObject& geometry) {
        const auto description = geometry.value("description").toObject();
        const int tw = description.value("texture_width").toInt(skin.width());
        const int th = description.value("texture_height").toInt(skin.height());
        if (skin.isNull() || tw <= 0 || th <= 0) return {};
        QList<QJsonObject> bones;
        for (const auto& bone : geometry.value("bones").toArray()) bones.append(bone.toObject());
        const auto isHead = [](const QJsonObject& bone) {
            const auto name = bone.value("name").toString().toLower();
            return name.contains("head") || name.contains("skull");
        };
        std::stable_sort(bones.begin(), bones.end(), [&](const auto& left, const auto& right) { return isHead(left) && !isHead(right); });
        for (const auto& bone : bones) {
            for (const auto& cubeValue : bone.value("cubes").toArray()) {
                const auto cube = cubeValue.toObject();
                const auto size = cube.value("size").toArray();
                if (size.size() < 3) continue;
                double x = 0, y = 0, w = 0, h = 0;
                const auto value = cube.value("uv");
                if (value.isArray()) {
                    const auto uv = value.toArray();
                    if (uv.size() < 2) continue;
                    const double d = size[2].toDouble();
                    x = uv[0].toDouble() + d;
                    y = uv[1].toDouble() + d;
                    w = size[0].toDouble();
                    h = size[1].toDouble();
                } else if (value.isObject()) {
                    const auto face = value.toObject().value("north").toObject();
                    const auto uv = face.value("uv").toArray();
                    const auto uvSize = face.value("uv_size").toArray();
                    if (uv.size() < 2 || uvSize.size() < 2) continue;
                    x = uv[0].toDouble();
                    y = uv[1].toDouble();
                    w = uvSize[0].toDouble();
                    h = uvSize[1].toDouble();
                }
                const QRectF crop =
                    QRectF(x * skin.width() / tw, y * skin.height() / th, w * skin.width() / tw, h * skin.height() / th).normalized();
                if (crop.isEmpty() || !QRectF(skin.rect()).contains(crop)) continue;
                const auto face = skin.copy(crop.toAlignedRect());
                if (face.isNull()) continue;
                QImage icon(32, 32, QImage::Format_ARGB32_Premultiplied);
                icon.fill(Qt::transparent);
                const QSize fitted = face.size().scaled(32, 32, Qt::KeepAspectRatio);
                QPainter painter(&icon);
                painter.drawImage(QRect((32 - fitted.width()) / 2, (32 - fitted.height()) / 2, fitted.width(), fitted.height()), face);
                return icon;
            }
        }
        return {};
    }
    QString defaultReference(const QJsonObject& object) {
        if (object.value("default").isString()) return object.value("default").toString();
        for (auto it = object.begin(); it != object.end(); ++it)
            if (it.value().isString()) return it.value().toString();
        return {};
    }
    void process(const QList<Pack>& packs, AddonResources& out) {
        QHash<QString, QJsonValue> items;
        QHash<QString, LocatedTexture> terrain, rawTextures;
        QHash<QString, QJsonObject> geometries;
        QHash<QString, QString> namesEn, namesZh;
        for (const auto& p : packs)
            if (p.type == "resources") {
                const auto item = textureTable(p, "textures/item_texture.json", &out.warnings);
                for (auto i = item.begin(); i != item.end(); ++i) items.insert(i.key(), i.value());
                const auto blocks = textureTable(p, "textures/terrain_texture.json", &out.warnings);
                for (auto i = blocks.begin(); i != blocks.end(); ++i) terrain.insert(i.key(), {&p, texturePath(i.value())});
                for (const auto& path : p.paths) {
                    if (path.startsWith("textures/") &&
                        (path.endsWith(".png", Qt::CaseInsensitive) || path.endsWith(".tga", Qt::CaseInsensitive))) {
                        rawTextures.insert(path, {&p, path});
                        rawTextures.insert(path.left(path.lastIndexOf('.')), {&p, path});
                    }
                    if ((path.startsWith("models/") || path.startsWith("model/")) && path.endsWith(".json", Qt::CaseInsensitive)) {
                        const auto object = json(p.get(path), &out.warnings, path).object();
                        for (auto it = object.begin(); it != object.end(); ++it) {
                            if (!it.key().startsWith("geometry.") || !it.value().isObject()) continue;
                            auto legacy = it.value().toObject();
                            QJsonObject description;
                            description.insert("identifier", it.key());
                            description.insert("texture_width", legacy.value("texturewidth").toInt(64));
                            description.insert("texture_height", legacy.value("textureheight").toInt(64));
                            legacy.insert("description", description);
                            geometries.insert(it.key(), legacy);
                        }
                        for (const auto& value : object.value("minecraft:geometry").toArray()) {
                            const auto geometry = value.toObject();
                            const auto id = geometry.value("description").toObject().value("identifier").toString();
                            if (!id.isEmpty()) geometries.insert(id, geometry);
                        }
                    }
                }
                const auto en = lang(p, "en_US"), zh = lang(p, "zh_CN");
                for (auto i = en.begin(); i != en.end(); ++i) namesEn.insert(i.key(), i.value());
                for (auto i = zh.begin(); i != zh.end(); ++i) namesZh.insert(i.key(), i.value());
            }
        QString locale = setting::current().language;
        if (locale.isEmpty() || locale == QStringLiteral("system"))
            locale = QLocale::system().language() == QLocale::Chinese ? "zh_CN" : "en_US";
        const bool chinese = locale.startsWith("zh", Qt::CaseInsensitive);
        QList<const Pack*> definitions;
        for (const auto& p : packs)
            if (p.type == "resources") definitions.append(&p);
        for (const auto& p : packs)
            if (p.type == "data") definitions.append(&p);
        for (const auto* pack : definitions) {
            const auto& p = *pack;
            if (p.type == "resources") {
                const auto blocks = json(p.get("blocks.json"), &out.warnings, "blocks.json").object();
                for (auto i = blocks.begin(); i != blocks.end(); ++i) {
                    if (!i.value().isObject()) continue;
                    const auto ref = textureReference(i.value().toObject().value("textures"));
                    const QColor color = average(locateImage(terrain, ref, rawTextures));
                    if (color.isValid()) out.blockColors.insert(i.key().contains(':') ? i.key() : "minecraft:" + i.key(), color);
                }
                for (const auto& path : p.paths)
                    if (path.startsWith("entity/") && path.endsWith(".json", Qt::CaseInsensitive)) {
                        const auto description = json(p.get(path), &out.warnings, path)
                                                     .object()
                                                     .value("minecraft:client_entity")
                                                     .toObject()
                                                     .value("description")
                                                     .toObject();
                        const QString id = description.value("identifier").toString();
                        if (id.isEmpty()) continue;
                        auto& entity = out.entities[id];
                        const QString key = "entity." + id + ".name";
                        entity.name = chinese ? namesZh.value(key, namesEn.value(key, id)) : namesEn.value(key, namesZh.value(key, id));
                        const auto egg = description.value("spawn_egg").toObject();
                        const QString eggTexture = egg.value("texture").toString();
                        if (!eggTexture.isEmpty()) {
                            const auto path = indexedTexture(items.value(eggTexture), egg.value("texture_index").toInt(0));
                            entity.icon = locateImage(rawTextures, path, rawTextures);
                        }
                        if (entity.icon.isNull()) {
                            const auto textures = description.value("textures").toObject();
                            const auto texture = defaultReference(textures);
                            const auto geometry = defaultReference(description.value("geometry").toObject());
                            entity.icon = cubeIcon(locateImage(rawTextures, texture, rawTextures), geometries.value(geometry));
                        }
                    }
            } else if (p.type == "data") {
                for (const auto& path : p.paths)
                    if (path.startsWith("entities/") && path.endsWith(".json", Qt::CaseInsensitive)) {
                        const auto description = json(p.get(path), &out.warnings, path)
                                                     .object()
                                                     .value("minecraft:entity")
                                                     .toObject()
                                                     .value("description")
                                                     .toObject();
                        const QString id = description.value("identifier").toString();
                        if (id.isEmpty()) continue;
                        auto& entity = out.entities[id];
                        if (entity.name.isEmpty()) {
                            const QString key = "entity." + id + ".name";
                            entity.name = chinese ? namesZh.value(key, namesEn.value(key, id)) : namesEn.value(key, namesZh.value(key, id));
                        }
                    }
                for (const auto& path : p.paths)
                    if (path.startsWith("blocks/") && path.endsWith(".json", Qt::CaseInsensitive)) {
                        const auto block = json(p.get(path), &out.warnings, path).object().value("minecraft:block").toObject();
                        const auto id = block.value("description").toObject().value("identifier").toString();
                        if (id.isEmpty()) continue;
                        const auto components = block.value("components").toObject();
                        const auto materials = components.value("minecraft:material_instances");
                        QString ref = textureReference(materials);
                        QColor color = average(locateImage(terrain, ref, rawTextures), blendedMaterial(materials));
                        if (!color.isValid())
                            for (const auto& permutation : block.value("permutations").toArray()) {
                                const auto material =
                                    permutation.toObject().value("components").toObject().value("minecraft:material_instances");
                                ref = textureReference(material);
                                color = average(locateImage(terrain, ref, rawTextures), blendedMaterial(material));
                                if (color.isValid()) break;
                            }
                        if (!color.isValid()) {
                            const auto mapColor = components.value("minecraft:map_color");
                            const QString text = mapColor.isString() ? mapColor.toString() : mapColor.toObject().value("color").toString();
                            color = QColor(text);
                        }
                        if (color.isValid()) out.blockColors.insert(id, color);
                    }
            }
        }
        for (auto it = out.entities.begin(); it != out.entities.end(); ++it) {
            if (it->icon.isNull()) continue;
            it->icon = it->icon.scaled(32, 32, Qt::KeepAspectRatio, Qt::FastTransformation);
            QByteArray png;
            QBuffer buffer(&png);
            buffer.open(QIODevice::WriteOnly);
            if (it->icon.save(&buffer, "PNG")) it->iconUrl = "data:image/png;base64," + QString::fromLatin1(png.toBase64());
        }
    }
    void worldPacks(const QString& world, QList<Pack>& packs, AddonResources& result) {
        struct Requirement {
            QString uuid, type;
            QJsonArray version;
        };
        QList<Requirement> required;
        for (const auto& type : {QStringLiteral("resource"), QStringLiteral("behavior")}) {
            const QString filename = "world_" + type + "_packs.json";
            const auto file = io::DocumentIo::stat(child(world, filename));
            if (!file.ok() || !file.info.exists) continue;
            const auto data = io::DocumentIo::readAll(child(world, filename));
            if (!data.ok()) {
                result.warnings << AddonResources::tr("Cannot read %1").arg(filename);
                continue;
            }
            const auto doc = json(data.data, &result.warnings, filename);
            if (!doc.isArray()) {
                result.warnings << AddonResources::tr("Invalid %1").arg(filename);
                continue;
            }
            const auto enabled = doc.array();
            for (qsizetype i = enabled.size(); i > 0; --i) {
                const auto obj = enabled[i - 1].toObject();
                const auto uuid = obj.value("pack_id").toString().toLower();
                if (!uuid.isEmpty()) required.append({uuid, type == "resource" ? "resources" : "data", obj.value("version").toArray()});
            }
        }
        if (required.isEmpty()) return;
        QList<Pack> candidates;
        const QDir worldDir(world);
        const QString mojang = worldDir.absoluteFilePath("../..");
        for (const QString& base :
             {worldDir.filePath("resource_packs"), worldDir.filePath("behavior_packs"), QDir(mojang).filePath("resource_packs"),
              QDir(mojang).filePath("behavior_packs"), QDir(mojang).filePath("development_resource_packs"),
              QDir(mojang).filePath("development_behavior_packs")}) {
            const auto list = io::DocumentIo::list(QUrl::fromLocalFile(base));
            if (!list.ok()) continue;
            for (const auto& name : list.names) {
                if (name == "." || name == ".." || name.contains('/') || name.contains('\\')) continue;
                const auto url = child(base, name);
                const auto status = io::DocumentIo::stat(url);
                if (!status.ok()) continue;
                if (status.info.isDirectory) {
                    Pack p;
                    p.root = url.toLocalFile();
                    if (manifest(p) && std::any_of(required.cbegin(), required.cend(), [&](const Requirement& req) {
                            return req.uuid == p.uuid && req.type == p.type && req.version == p.version;
                        }))
                        candidates << std::move(p);
                } else if (name.endsWith(".mcpack", Qt::CaseInsensitive) || name.endsWith(".zip", Qt::CaseInsensitive))
                    addSource(url, candidates, result.warnings);
            }
        }
        for (const auto& req : required) {
            bool found = false;
            for (const auto& candidate : candidates)
                if (candidate.uuid == req.uuid && candidate.type == req.type && candidate.version == req.version) {
                    Pack selected = candidate;
                    if (!selected.root.isEmpty()) scanDir(selected.root, {}, selected.paths);
                    packs << std::move(selected);
                    found = true;
                    break;
                }
            if (!found) result.warnings << AddonResources::tr("Enabled pack not found: %1").arg(req.uuid);
        }
    }
}  // namespace

std::shared_ptr<AddonResources> AddonResources::load(const QList<QUrl>& sources, const QString& worldPath) {
    auto result = std::make_shared<AddonResources>();
    QList<Pack> packs;
    if (!worldPath.isEmpty()) worldPacks(worldPath, packs, *result);
    for (const auto& source : sources) {
        const auto count = packs.size();
        addSource(source, packs, result->warnings);
        if (packs.size() == count) {
            result->error = AddonResources::tr("No resource or behavior pack was found in %1.").arg(io::DocumentIo::displayName(source));
            return result;
        }
    }
    QList<Pack> active;
    QSet<QString> selected;
    for (qsizetype i = packs.size(); i > 0; --i) {
        const auto& pack = packs[i - 1];
        const auto key = pack.uuid + QLatin1Char('/') + pack.type;
        if (selected.contains(key)) continue;
        selected.insert(key);
        active.prepend(pack);
    }
    result->packCount = active.size();
    process(active, *result);
    return result;
}
