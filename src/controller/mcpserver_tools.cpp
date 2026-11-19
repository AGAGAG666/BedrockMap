#include "mcpserver_p.h"

#include <QCryptographicHash>
#include <QDir>
#include <QJsonDocument>
#include <QMetaProperty>
#include <QPointer>
#include <QUuid>
#include <algorithm>
#include <cmath>
#include <limits>

#include "base/config.h"
#include "controller/appcontroller.h"
#include "controller/mcstructuresession.h"
#include "controller/nbtsession.h"
#include "loader/leveloperator.h"
#include "model/nbttreemodel.h"

namespace {
using Object = QJsonObject;
using Array = QJsonArray;
using Value = QJsonValue;

struct ToolError { QString message; };
void require(bool valid, const QString& message) { if (!valid) throw ToolError{message}; }

Object result(const char* state = "completed") { return {{"state", state}}; }
QString string(const Object& args, const char* key) { return args.value(key).toString(); }
int integer(const Object& args, const char* key, int fallback = 0) { return args.value(key).toInt(fallback); }
bool boolean(const Object& args, const char* key, bool fallback = false) { return args.value(key).toBool(fallback); }
Value json(const QVariant& v) { return Value::fromVariant(v); }

QUrl file(const Object& args, const char* key = "path") {
    const auto path = string(args, key);
    if (QDir::isAbsolutePath(path)) return QUrl::fromLocalFile(path);
    const QUrl url(path, QUrl::StrictMode);
    require(url.isValid() && (url.scheme() == "content" || (url.isLocalFile() && QDir::isAbsolutePath(url.toLocalFile()))),
            "Use an absolute device path or an accessible content URI.");
    return url;
}

bool matches(const Value& value, const QString& type) {
    if (type == "integer") {
        const auto x = value.toDouble(std::numeric_limits<double>::quiet_NaN());
        return std::isfinite(x) && std::trunc(x) == x && x >= std::numeric_limits<int>::min() && x <= std::numeric_limits<int>::max();
    }
    if (type == "boolean") return value.isBool();
    if (type == "number") return value.isDouble() && std::isfinite(value.toDouble());
    if (type == "string") return value.isString();
    if (type == "array") return value.isArray();
    if (type == "object") return value.isObject();
    return false;
}

Array vector(const QVector3D& v) { return {v.x(), v.y(), v.z()}; }
QVector3D vector(const Object& args, const char* key) {
    const auto a = args.value(key).toArray();
    require(a.size() == 3, QStringLiteral("%1 must contain three integers.").arg(QString::fromLatin1(key)));
    for (const auto& value : a) require(matches(value, "integer") && std::abs(value.toDouble()) <= 32000000, "Invalid volume coordinate.");
    return {float(a[0].toInt()), float(a[1].toInt()), float(a[2].toInt())};
}

int regionCoordinate(const Object& args, const char* key) {
    const auto value = integer(args, key);
    require(value >= -2000000 && value <= 2000000, "Chunk coordinates must be within ±2000000.");
    return value;
}

int offset(const Object& args) { return std::max(0, integer(args, "offset")); }
int limit(const Object& args) { return std::clamp(integer(args, "limit", 100), 1, 500); }
Object page(const QAbstractItemModel& model, const Object& args, const QSet<int>& excluded = {}) {
    const auto start = std::min(offset(args), model.rowCount());
    const auto end = start + std::min(limit(args), model.rowCount() - start);
    const auto roles = model.roleNames();
    Array rows;
    for (int i = start; i < end; ++i) {
        Object entry{{"row", i}};
        for (auto role = roles.cbegin(); role != roles.cend(); ++role)
            if (!excluded.contains(role.key())) entry.insert(QString::fromUtf8(role.value()), json(model.data(model.index(i, 0), role.key())));
        rows.append(entry);
    }
    return {{"items", rows}, {"total", model.rowCount()}, {"nextOffset", end < model.rowCount() ? Value(end) : Value()}};
}

QString selectionToken(const MapController& map) {
    Array state{map.dimension()};
    for (const auto& rect : map.selectionRects()) state.append(Array{rect.left(), rect.top(), rect.right(), rect.bottom()});
    return QString::fromLatin1(QCryptographicHash::hash(QJsonDocument(state).toJson(QJsonDocument::Compact), QCryptographicHash::Sha256).toHex());
}

Object sessionInfo(const WorkspaceSession& session, int index) {
    return {{"session", session.mcpId()}, {"index", index}, {"kind", session.kind()}, {"path", session.path()},
            {"title", session.title()}, {"dirty", session.isDirty()}, {"saving", session.saving()}, {"active", session.active()}};
}

WorkspaceSession* session(AppController& app, const Object& args) {
    const auto id = string(args, "session");
    const auto model = app.openSessions();
    for (int i = 0; i < model->count() && !id.isEmpty(); ++i)
        if (auto* found = model->at(i); found && found->mcpId() == id) return found;
    throw ToolError{"Session no longer exists. Call list_sessions."};
}

WorldSession* world(AppController& app, const Object& args, bool mustBeReady = true) {
    auto* target = qobject_cast<WorldSession*>(session(app, args));
    require(target && (!mustBeReady || target->ready()), "A ready world session is required.");
    return target;
}

bool busy(WorldSession* target) {
    auto* volume = target->voxelView();
    return target->saving() || target->mapController()->operationRunning() || target->deletingEntityRegion() ||
           target->chunkEditor()->loading() || volume->loading() || volume->deleting() || volume->importing() || volume->copying();
}

WorldSession* editableWorld(AppController& app, const Object& args) {
    auto* target = world(app, args);
    require(!busy(target), "Wait for the active world operation to finish.");
    return target;
}

GlobalNbtSection* section(WorldSession* target, const Object& args) {
    const auto sections = target->globalData()->sections();
    const auto index = integer(args, "section", -1);
    require(index >= 0 && index < sections.size(), "Invalid global NBT section.");
    auto* found = qobject_cast<GlobalNbtSection*>(sections.at(index).value<QObject*>());
    require(found, "Global NBT section unavailable.");
    return found;
}

NbtDocumentController* document(AppController& app, const Object& args, bool writing = false) {
    auto* target = session(app, args);
    const auto source = string(args, "source");
    NbtDocumentController* doc = nullptr;
    if (auto* standalone = qobject_cast<NbtSession*>(target); standalone && (source.isEmpty() || source == "file")) doc = standalone->document();
    if (auto* w = qobject_cast<WorldSession*>(target); w && w->ready()) {
        require(!writing || !busy(w), "Wait for the active world operation to finish.");
        auto* chunk = w->chunkEditor();
        if (source == "global") doc = section(w, args)->document();
        else if (chunk->hasChunk() && !chunk->loading()) {
            if (source == "entities") doc = chunk->entitiesDocument();
            else if (source == "block_entities") doc = chunk->blockEntitiesDocument();
            else if (source == "pending_ticks") doc = chunk->pendingTicksDocument();
        }
    }
    if (auto* structure = qobject_cast<McstructureSession*>(target); structure && source == "structure") doc = structure->controller()->document();
    require(doc && doc->hasDocument(), "No matching NBT document is open.");
    require(!writing || !doc->readOnly(), "This NBT document is read only.");
    return doc;
}

VoxelSceneController* voxel(AppController& app, const Object& args) {
    auto* target = session(app, args);
    const auto source = string(args, "source");
    if (auto* w = qobject_cast<WorldSession*>(target); w && w->ready()) {
        if (source == "chunk") return w->chunkEditor();
        if (source.isEmpty() || source == "world") return w->voxelView();
        if (source == "global") {
            if (auto* preview = section(w, args)->structurePreview()) return preview;
        }
    }
    if (auto* structure = qobject_cast<McstructureSession*>(target); structure && (source.isEmpty() || source == "structure"))
        return structure->controller();
    throw ToolError{"No matching 3D preview is available."};
}

BlockVolumeController* volume(AppController& app, const Object& args) {
    auto* target = qobject_cast<BlockVolumeController*>(voxel(app, args));
    require(target, "This operation needs a block volume preview.");
    return target;
}
}

McpServer::Tools::Tools(AppController& app) : app_(app) {
    add("get_job", "Get the result of an asynchronous operation.", "job:string", false, [this](const Object& a) {
        const auto it = jobs_.constFind(string(a, "job"));
        require(it != jobs_.cend(), "Job not found.");
        return it->state;
    });
    registerAppTools();
    registerWorldTools();
    registerDocumentTools();
}

void McpServer::Tools::add(const char* name, const char* description, const char* arguments, bool write,
                          std::function<Value(const Object&)> action) {
    Object properties;
    Array required;
    for (auto entry : QString::fromLatin1(arguments).split(' ', Qt::SkipEmptyParts)) {
        const bool optional = entry.endsWith('?');
        if (optional) entry.chop(1);
        const auto key = entry.section(':', 0, 0);
        const auto type = entry.section(':', 1, 1).section('=', 0, 0);
        Object field{{"type", type}};
        const auto options = entry.section('=', 1);
        if (!options.isEmpty()) field.insert("enum", Array::fromStringList(options.split('|')));
        properties.insert(key, field);
        if (!optional) required.append(key);
    }
    tools_.insert(QString::fromLatin1(name), Tool{Object{{"name", QString::fromLatin1(name)}, {"description", QString::fromUtf8(description)},
        {"annotations", Object{{"readOnlyHint", !write}}}, {"inputSchema", Object{{"type", "object"}, {"properties", properties}, {"required", required}, {"additionalProperties", false}}}},
        write, std::move(action)});
}

Array McpServer::Tools::list() const {
    Array list;
    for (const auto& tool : tools_) list.append(tool.schema);
    return list;
}

Object McpServer::Tools::call(const Object& params, bool writeAllowed) {
    Value value;
    bool error = false;
    try {
        const auto it = tools_.constFind(string(params, "name"));
        require(it != tools_.cend(), "Unknown BedrockMap tool.");
        require(!it->write || writeAllowed, "Enable MCP write access in BedrockMap before running this tool.");
        const auto args = params.value("arguments").toObject();
        const auto schema = it->schema.value("inputSchema").toObject();
        const auto properties = schema.value("properties").toObject();
        for (const auto& key : schema.value("required").toArray())
            require(args.contains(key.toString()), "Missing argument: " + key.toString());
        for (auto arg = args.begin(); arg != args.end(); ++arg) {
            const auto field = properties.value(arg.key()).toObject();
            require(matches(arg.value(), field.value("type").toString()), "Invalid argument: " + arg.key());
            if (field.contains("enum")) require(field.value("enum").toArray().contains(arg.value()), "Invalid option: " + arg.key());
        }
        value = it->action(args);
    } catch (const ToolError& e) {
        error = true;
        value = Object{{"error", e.message}};
    } catch (const std::exception&) {
        error = true;
        value = Object{{"error", "BedrockMap could not complete this operation."}};
    }
    const auto object = value.isObject() ? value.toObject() : Object{{"value", value}};
    return {{"content", Array{Object{{"type", "text"}, {"text", QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact))}}}},
            {"structuredContent", object}, {"isError", error}};
}

QString McpServer::Tools::startJob(QObject* owner) {
    if (jobs_.size() >= 128) {
        auto it = std::find_if(jobs_.begin(), jobs_.end(), [](const Job& job) { return job.observer == nullptr; });
        require(it != jobs_.end(), "Too many unfinished jobs.");
        jobs_.erase(it);
    }
    const auto id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    auto* observer = new QObject(this);
    jobs_.insert(id, Job{Object{{"job", id}, {"state", "running"}}, observer});
    connect(owner, &QObject::destroyed, observer, [this, id] { finishJob(id, "The workspace closed before completion."); });
    return id;
}

void McpServer::Tools::finishJob(const QString& id, const QString& error) {
    auto it = jobs_.find(id);
    if (it == jobs_.end() || !it->observer) return;
    it->state.insert("state", error.isEmpty() ? "completed" : "failed");
    if (!error.isEmpty()) it->state.insert("error", error);
    it->observer->deleteLater();
    it->observer = nullptr;
}

void McpServer::Tools::registerAppTools() {
    add("list_worlds", "List discovered or recent worlds, with device paths.", "recent:boolean? offset:integer? limit:integer?", false,
        [this](const Object& a) {
            const auto* model = boolean(a, "recent") ? app_.worlds()->recentWorlds() : app_.worlds()->allWorlds();
            auto data = page(*model, a, {WorldMetadataListModel::ThumbnailRole});
            data.insert("loading", app_.worlds()->loading());
            return data;
        });
    add("refresh_worlds", "Rescan the configured world folders.", "", false, [this](const Object&) {
        app_.worlds()->refresh(); return result("requested");
    });
    add("list_sessions", "List the open BedrockMap sessions and their stable session IDs.", "", false, [this](const Object&) {
        Array found;
        const auto* model = app_.openSessions();
        for (int i = 0; i < model->count(); ++i) if (const auto* s = model->at(i)) found.append(sessionInfo(*s, i));
        return Object{{"items", found}};
    });
    add("select_session", "Activate an open workspace.", "session:string", false, [this](const Object& a) {
        app_.selectWorkspace(app_.openSessions()->indexOf(session(app_, a))); return result();
    });
    add("open_world", "Open a world folder. Use a discovered device path or content URI.", "path:string", true, [this](const Object& a) {
        app_.openWorldFolder(file(a)); return result("requested");
    });
    add("open_file", "Open an NBT or mcstructure file, or import a mcworld or addon archive by extension.", "path:string", true,
        [this](const Object& a) { app_.openFile(file(a)); return result("requested"); });
    add("new_nbt", "Create a blank NBT document.", "", true, [this](const Object&) { app_.newNbtDocument(); return result(); });
    add("close_session", "Close a workspace. Set discard to true to discard unsaved changes.", "session:string discard:boolean?", true,
        [this](const Object& a) {
            auto* s = session(app_, a);
            require(!s->saving(), "Wait for the save to finish.");
            if (auto* w = qobject_cast<WorldSession*>(s)) require(!busy(w), "Wait for the active operation to finish.");
            require(!s->isDirty() || boolean(a, "discard"), "The workspace has unsaved changes. Save it or pass discard=true.");
            app_.discardSession(app_.openSessions()->indexOf(s)); return result();
        });
    add("save_session", "Save the workspace to its existing destination. Use nbt_save_as for an untitled file.", "session:string", true,
        [this](const Object& a) {
            auto* s = session(app_, a);
            require(s->titleBarSaveEnabled() && !s->saving(), "This workspace is not ready to save.");
            if (auto* w = qobject_cast<WorldSession*>(s)) {
                require(!busy(w) && !w->chunkEditor()->dirty(), "Apply chunk editor changes and wait for the active operation before saving.");
                require(!w->mapController()->regionImport()->active() && !w->mapController()->regionImport()->loading(), "Finish the region import preview first.");
            }
            if (auto* nbt = qobject_cast<NbtSession*>(s)) require(!nbt->document()->requiresSaveAs(), "Use nbt_save_as for an untitled document.");
            const auto id = startJob(s);
            auto* observer = jobs_.value(id).observer;
            connect(s, &WorkspaceSession::saved, observer, [this, id] { finishJob(id); });
            connect(s, &WorkspaceSession::saveFailed, observer, [this, id](const QString& error) { finishJob(id, error); });
            if (!s->commit()) finishJob(id, "The workspace rejected the save.");
            else if (!s->saving()) finishJob(id);
            return jobs_.value(id).state;
        });
    add("world_file_operation", "Import a mcworld archive, copy a world folder into the app, export a closed world or delete a closed internal world.",
        "operation:string=import|copy|export|delete path:string destination:string?", true, [this](const Object& a) {
            const auto action = string(a, "operation");
            require(!app_.deletingWorld() && !app_.importingWorld() && !app_.exportingWorld(), "Another world file operation is active.");
            const auto url = file(a);
            QUrl destination;
            if (action == "export") destination = file(a, "destination");
            require(action == "import" || action == "copy" || action == "export" || action == "delete", "Unknown world file operation.");
            require((action != "export" && action != "delete") || url.isLocalFile(), "Use a local world folder for export or delete.");
            if (action == "delete") require(LevelOperator::isInternalWorld(url.toLocalFile()), "Only internal worlds can be deleted.");
            const auto id = startJob(&app_);
            auto* observer = jobs_.value(id).observer;
            connect(&app_, &AppController::worldFileOperationFinished, observer, [this, id](const QString& error) { finishJob(id, error); });
            if (action == "import") app_.openMcworldFile(url);
            else if (action == "copy") app_.copyWorldToInternal(url);
            else if (action == "export") app_.exportWorld(url.toLocalFile(), destination);
            else app_.deleteWorld(url.toLocalFile());
            if (!app_.deletingWorld() && !app_.importingWorld() && !app_.exportingWorld()) {
                if (action == "import" || action == "copy") {
                    jobs_[id].state.insert("state", "requested");
                    observer->deleteLater(); jobs_[id].observer = nullptr;
                } else finishJob(id, "Operation did not start. Verify the path and close any workspace in that world.");
            }
            return jobs_.value(id).state;
        });
    add("addon_status", "Read the loaded addon count and import status.", "", false, [this](const Object&) {
        return Object{{"packs", app_.addonPackCount()}, {"loading", app_.importingAddon()}};
    });
    add("import_addon", "Import an addon resource archive.", "path:string", true, [this](const Object& a) {
        require(!app_.importingAddon(), "An addon import is already running.");
        const auto url = file(a);
        const auto id = startJob(&app_);
        connect(&app_, &AppController::addonImportFinished, jobs_.value(id).observer,
                [this, id](const QString& error) { finishJob(id, error); });
        app_.importAddonResource(url); return jobs_.value(id).state;
    });
    add("list_log_history", "List retained historical log filenames.", "", false, [this](const Object&) {
        return Object{{"items", json(app_.logs()->historicalFiles())}};
    });
    add("read_logs", "Page through the current log view. Pass offset and limit to navigate.", "offset:integer? limit:integer?", false,
        [this](const Object& a) {
            auto data = page(*app_.logs(), a);
            auto entries = data.value("items").toArray();
            for (auto it = entries.begin(); it != entries.end(); ++it) {
                auto entry = (*it).toObject();
                const auto line = entry.value("logText").toString();
                if (line.contains("leveldb_xor_key", Qt::CaseInsensitive)) entry.insert("logText", "[redacted]");
                *it = entry;
            }
            data.insert("items", entries); return data;
        });
    add("export_logs", "Export a current or historical log file to a device destination.", "destination:string historyName:string?", true,
        [this](const Object& a) {
            const auto url = file(a, "destination");
            const auto name = string(a, "historyName");
            const bool ok = app_.logs()->saveForMcp(name, url);
            require(ok, app_.logs()->error()); return result();
        });
    add("set_log_view", "Set the log filter, show or hide prefixes, choose the minimum level (0-3), or clear the displayed log entries.",
        "filter:string? prefix:boolean? minimumLevel:integer? clear:boolean?", true, [this](const Object& a) {
            auto* logs = app_.logs();
            if (a.contains("minimumLevel")) require(integer(a, "minimumLevel") >= 0 && integer(a, "minimumLevel") <= 3, "Invalid log level.");
            if (a.contains("filter")) logs->setFilter(string(a, "filter"));
            if (a.contains("prefix")) logs->setShowPrefix(boolean(a, "prefix"));
            if (a.contains("minimumLevel")) logs->setMinimumLevel(integer(a, "minimumLevel"));
            if (boolean(a, "clear")) logs->clear();
            return result();
        });
    add("get_app_status", "Read file, permissions and update status.", "", false, [this](const Object&) {
        return Object{{"importingWorld", app_.importingWorld()}, {"exportingWorld", app_.exportingWorld()},
                      {"deletingWorld", app_.deletingWorld()}, {"worldTask", app_.worldTaskTitle()},
                      {"checkingForUpdates", app_.checkingForUpdates()}, {"allFilesPermission", app_.allFilesPermission()},
                      {"shizukuPermission", app_.shizukuPermission()}, {"rootPermission", app_.rootPermission()}};
    });
    add("check_for_updates", "Request an update check in BedrockMap.", "", true, [this](const Object&) {
        app_.checkForUpdates(true); return result("requested");
    });
    add("get_settings", "Read BedrockMap settings. The world decryption key is local only.", "", false, [this](const Object&) {
        auto* settings = app_.settings();
        const auto* type = settings->metaObject();
        Object values;
        for (int i = type->propertyOffset(); i < type->propertyCount(); ++i) {
            const auto property = type->property(i);
            if (QByteArrayView(property.name()) != "leveldbXorKey") values.insert(QString::fromLatin1(property.name()), json(property.read(settings)));
        }
        return values;
    });
    add("set_setting", "Change a writable BedrockMap setting using the same setter as the settings page.", "name:string value:object", true,
        [this](const Object& a) {
            const auto name = string(a, "name").toLatin1();
            auto* settings = app_.settings();
            const auto* type = settings->metaObject();
            const auto index = type->indexOfProperty(name.constData());
            require(index >= type->propertyOffset() && name != "leveldbXorKey", "Setting is unavailable through MCP.");
            const auto property = type->property(index);
            require(property.isWritable(), "Setting is read only.");
            const auto v = a.value("value").toObject().value("data");
            const auto target = property.metaType();
            const auto kind = target.id() == QMetaType::Bool ? "boolean" : target.id() == QMetaType::Int ? "integer" :
                              target.id() == QMetaType::Double ? "number" : target.id() == QMetaType::QString ? "string" : "";
            require(matches(v, QString::fromLatin1(kind)), "Supply value as {data: <setting value>} with the type returned by get_settings.");
            require(property.write(settings, v.toVariant()), "The setting rejected that value.");
            return Object{{"value", json(property.read(settings))}};
        });
    add("settings_action", "Save settings, reset settings, or add or remove a custom world folder.", "operation:string=save|reset|add_world_folder|remove_world_folder path:string?", true,
        [this](const Object& a) {
            auto* settings = app_.settings();
            const auto action = string(a, "operation");
            if (action == "save") require(settings->save(), "Could not save settings.");
            else if (action == "reset") settings->reset();
            else if (action == "add_world_folder" || action == "remove_world_folder") {
                const auto path = file(a).toLocalFile();
                require(!path.isEmpty(), "Expected a local folder path.");
                if (action == "add_world_folder") settings->addCustomWorldPath(path);
                else settings->removeCustomWorldPath(path);
                require(settings->saveWorldSources(), "Could not save the world folders.");
            } else throw ToolError{"Unknown settings operation."};
            return result();
        });
}

void McpServer::Tools::registerWorldTools() {
    add("get_world", "Read world readiness, map camera, operation state and current selection.", "session:string", false, [this](const Object& a) {
        auto* w = world(app_, a, false);
        auto* map = w->mapController();
        const auto camera = map->cameraState();
        Object out = sessionInfo(*w, app_.openSessions()->indexOf(w));
        Array selection;
        for (const auto& r : map->selectionRects()) selection.append(Array{r.left(), r.top(), r.right(), r.bottom()});
        out.insert("ready", w->ready()); out.insert("indexLoading", w->indexLoading()); out.insert("error", w->errorCode());
        out.insert("operationRunning", map->operationRunning()); out.insert("dimension", camera.dimension);
        out.insert("selection", selection); out.insert("selectionChunkCount", map->selectionChunkCount());
        out.insert("selectionToken", selectionToken(*map));
        out.insert("centerX", camera.center.x() * 16.0); out.insert("centerZ", camera.center.y() * 16.0);
        out.insert("pixelsPerChunk", camera.pixelsPerChunk); out.insert("overlays", json(map->overlays()));
        return out;
    });
    add("navigate_map", "Jump to block coordinates. Pass a dimension or map zoom in pixels per chunk if needed.",
        "session:string x:integer z:integer dimension:integer? zoom:number?", false, [this](const Object& a) {
            auto* w = editableWorld(app_, a);
            auto* map = w->mapController();
            require(!map->regionImport()->active() && !map->regionImport()->loading(), "Finish the region import preview first.");
            if (a.contains("zoom")) require(a.value("zoom").toDouble() > 0, "Zoom must be positive.");
            app_.selectWorkspace(app_.openSessions()->indexOf(w)); w->setWorkspaceSection(0); w->setWorkspaceRoute("root");
            if (a.contains("dimension")) map->setDimension(integer(a, "dimension"));
            if (a.contains("zoom")) map->setScale(a.value("zoom").toDouble());
            map->gotoBlock(integer(a, "x"), integer(a, "z")); return result();
        });
    add("inspect_map_block", "Read block, biome, altitude and teleport coordinates from the map cache in the current dimension.",
        "session:string x:integer z:integer", false, [this](const Object& a) {
            return json(world(app_, a)->mapController()->blockInfo(integer(a, "x"), integer(a, "z")));
        });
    add("capture_map", "Save a screenshot of the visible world map to a device destination.",
        "session:string destination:string", true, [this](const Object& a) {
            auto* w = world(app_, a);
            require(app_.activeSession() == w && app_.workspaceOpen() && w->workspaceSection() == 0,
                    "Open the world map in BedrockMap before capturing it.");
            const auto url = file(a, "destination");
            const auto id = startJob(w);
            connect(&app_, &AppController::mapCaptureFinished, jobs_.value(id).observer,
                    [this, id](const QString& captureId, const QString& error) { if (captureId == id) finishJob(id, error); });
            if (!app_.captureMap(id, w->mapController(), url)) finishJob(id, "Another map capture is pending.");
            return jobs_.value(id).state;
        });
    add("map_display", "Set the map layer (0 terrain, 1 biomes), transparent void or an overlay (0 grid, 1 coordinates, 2 slime, 3 entities, 4 villages, 5 spawn areas).",
        "session:string layer:integer? transparentVoid:boolean? overlay:integer? visible:boolean?", false, [this](const Object& a) {
            auto* map = world(app_, a)->mapController();
            if (a.contains("layer")) require(integer(a, "layer") >= 0 && integer(a, "layer") <= 1, "Invalid map layer.");
            if (a.contains("overlay")) require(integer(a, "overlay") >= 0 && integer(a, "overlay") < MapRenderOptions::OtherLen && a.contains("visible"), "Invalid map overlay.");
            if (a.contains("layer")) map->setLayer(integer(a, "layer"));
            if (a.contains("transparentVoid")) map->setTransparentVoid(boolean(a, "transparentVoid"));
            if (a.contains("overlay")) map->setOther(integer(a, "overlay"), boolean(a, "visible"));
            return result();
        });
    add("map_options", "Read the available biomes, dimensions and current map filter.", "session:string", false, [this](const Object& a) {
        auto* map = world(app_, a)->mapController();
        return Object{{"biomes", json(map->biomeOptions())}, {"customDimensions", json(map->customDimensionOptions())},
                      {"filter", json(map->mapFilterOptions())}, {"defaultFilter", json(map->defaultMapFilterOptions())}};
    });
    add("set_map_filter", "Apply map filter options using the shape returned by map_options.", "session:string filter:object", false,
        [this](const Object& a) {
            require(editableWorld(app_, a)->mapController()->applyMapFilterOptions(a.value("filter").toObject().toVariantMap()), "Could not set filter.");
            return result();
        });
    add("select_chunks", "Replace the current selection with inclusive rectangles [minX,minZ,maxX,maxZ] in chunk coordinates, or clear it with an empty list.",
        "session:string rectangles:array", false, [this](const Object& a) {
            auto* map = editableWorld(app_, a)->mapController();
            QRegion selected;
            const auto rectangles = a.value("rectangles").toArray();
            require(rectangles.size() <= 1024, "Too many selection rectangles.");
            for (const auto& item : rectangles) {
                const auto r = item.toArray();
                require(r.size() == 4, "Each rectangle must contain four chunk coordinates.");
                for (const auto& c : r) require(matches(c, "integer") && std::abs(c.toDouble()) <= 2000000, "Invalid chunk coordinate.");
                const auto left = r[0].toInt(), top = r[1].toInt(), right = r[2].toInt(), bottom = r[3].toInt();
                require(left <= right && top <= bottom, "Rectangle limits are reversed.");
                selected += QRect(QPoint(left, top), QPoint(right, bottom));
            }
            qint64 count = 0;
            for (const auto& r : selected) count += qint64(r.width()) * r.height();
            require(count <= std::numeric_limits<int>::max(), "Selection exceeds the chunk count range.");
            map->replaceSelection(selected); return Object{{"chunks", count}, {"selectionToken", selectionToken(*map)}};
        });
    add("edit_map_selection", "Copy, export, delete chunks, delete entities, delete outside, create void chunks, or set the biome of the current selection.",
        "session:string operation:string=copy|export|delete_chunks|delete_entities|delete_outside|create_void|set_biome dimension:integer selectionToken:string biome:integer? destination:string?", true, [this](const Object& a) {
            auto* w = editableWorld(app_, a);
            auto* map = w->mapController();
            require(map->dimension() == integer(a, "dimension") && map->hasSelection() && !map->selectionDragging() &&
                    selectionToken(*map) == string(a, "selectionToken"), "The selection changed. Call get_world or select_chunks.");
            require(!w->chunkEditor()->dirty() && !map->regionImport()->active() && !map->regionImport()->loading(), "Finish the current chunk edit or import first.");
            const auto action = string(a, "operation");
            std::function<bool()> apply;
            if (action == "copy") apply = [map] { return map->copySelectionAsync(); };
            else if (action == "export") { const auto url = file(a, "destination"); apply = [map, url] { return map->exportSelectionAsync(url); }; }
            else if (action == "delete_chunks") apply = [map] { return map->deleteSelectionAsync(); };
            else if (action == "delete_entities") apply = [map] { return map->deleteSelectionEntitiesAsync(); };
            else if (action == "delete_outside") apply = [map] { return map->deleteOutsideSelectionAsync(); };
            else if (action == "create_void") apply = [map] { return map->createVoidSelectionAsync(); };
            else if (action == "set_biome") {
                const auto biome = integer(a, "biome", -1);
                require(biome >= 0, "A biome ID is required.");
                apply = [map, biome] { return map->setSelectionBiomeAsync(biome); };
            } else throw ToolError{"Unknown selection operation."};
            const auto id = startJob(map);
            auto* observer = jobs_.value(id).observer;
            connect(map, &MapController::operationFinished, observer, [this, id] { finishJob(id); });
            connect(map, &MapController::operationFailed, observer, [this, id](const QString& error) { finishJob(id, error); });
            if (!apply()) finishJob(id, "BedrockMap rejected the selection operation.");
            return jobs_.value(id).state;
        });
    add("map_import", "Load, paste, move, cancel or apply a region import preview (chunk coordinates).",
        "session:string operation:string=load|paste|move|cancel|apply path:string? x:integer? z:integer?", true, [this](const Object& a) {
            auto* w = editableWorld(app_, a);
            auto* map = w->mapController();
            auto* preview = map->regionImport();
            const auto action = string(a, "operation");
            if (action == "cancel") { map->cancelImport(); return result(); }
            if (action == "move") {
                require(preview->active() && a.contains("x") && a.contains("z"), "An active preview and target coordinates are required.");
                const auto x = regionCoordinate(a, "x"), z = regionCoordinate(a, "z");
                preview->setTargetX(x); preview->setTargetZ(z); return result();
            }
            require(!w->chunkEditor()->dirty(), "Apply or discard the current chunk edit first.");
            if (action == "load" || action == "paste") {
                require(a.contains("x") && a.contains("z") && !preview->loading(), "Target coordinates and an idle preview are required.");
                const auto x = regionCoordinate(a, "x"), z = regionCoordinate(a, "z");
                const auto url = action == "load" ? file(a) : QUrl{};
                const auto id = startJob(preview);
                auto* observer = jobs_.value(id).observer;
                connect(preview, &RegionImportController::failed, observer, [this, id](const QString& error) { finishJob(id, error); });
                connect(preview, &RegionImportController::activeChanged, observer, [this, id, preview] { if (preview->active()) finishJob(id); });
                connect(preview, &RegionImportController::loadingChanged, observer, [this, id, preview] {
                    if (!preview->loading() && !preview->active()) finishJob(id, "The import preview did not load.");
                }, Qt::QueuedConnection);
                if (!(action == "load" ? map->beginImportAsync(url, x, z) : map->beginPasteAsync(x, z))) finishJob(id, "Import preview could not start.");
                return jobs_.value(id).state;
            }
            require(action == "apply" && preview->active(), "An active import preview is required.");
            const auto id = startJob(map);
            auto* observer = jobs_.value(id).observer;
            connect(map, &MapController::operationFinished, observer, [this, id] { finishJob(id); });
            connect(map, &MapController::operationFailed, observer, [this, id](const QString& error) { finishJob(id, error); });
            if (!map->confirmImport()) finishJob(id, "The import could not start.");
            return jobs_.value(id).state;
        });
    add("scan_entities", "Start or cancel the selected world's background entity scan.", "session:string cancel:boolean?", false,
        [this](const Object& a) {
            auto* w = world(app_, a);
            auto* e = w->worldEntities();
            if (boolean(a, "cancel")) { if (e->loading()) e->cancel(); return result("requested"); }
            require(!busy(w) && !e->loading(), "Wait for the active scan or edit to finish.");
            return [this, e] {
                const auto id = startJob(e);
                connect(e, &WorldEntityController::stateChanged, jobs_.value(id).observer, [this, id, e] {
                    if (!e->loading()) finishJob(id, e->error().isEmpty() && e->loaded() && !e->stale() && !e->scanCancelled() ? QString{} :
                                                      e->error().isEmpty() ? QStringLiteral("The scan was cancelled or the results are stale.") : e->error());
                });
                e->refresh(); return jobs_.value(id).state;
            }();
        });
    add("entity_scan_status", "Read entity scan progress, counts and any error.", "session:string", false, [this](const Object& a) {
        auto* e = world(app_, a, false)->worldEntities();
        return Object{{"loading", e->loading()}, {"loaded", e->loaded()}, {"stale", e->stale()}, {"error", e->error()},
                      {"progress", e->progressText()}, {"entityCount", double(e->entityCount())}, {"scannedKeys", double(e->scannedKeys())},
                      {"skippedRecords", double(e->skippedRecords())}, {"regionCount", e->regionCount()}};
    });
    add("list_entity_regions", "Page through scanned 16 by 16 block entity regions. Positions start at startX and startZ, in blocks.",
        "session:string offset:integer? limit:integer?", false, [this](const Object& a) {
            const auto* e = world(app_, a)->worldEntities();
            require(e->loaded() && !e->stale() && !e->loading(), "Finish a current entity scan first.");
            return page(*e, a);
        });
    add("list_region_entities", "Page through entities in one scanned region. x and z are chunk coordinates (floor(block coordinate / 16)).",
        "session:string dimension:integer x:integer z:integer offset:integer? limit:integer?", false, [this](const Object& a) {
            const auto* e = world(app_, a)->worldEntities();
            require(e->loaded() && !e->stale() && !e->loading(), "Finish a current entity scan first.");
            const auto row = e->findRegion({integer(a, "x"), integer(a, "z"), integer(a, "dimension")});
            require(row.has_value(), "No scanned entity region at those coordinates.");
            const auto list = e->entitiesForRegionPage(*row, offset(a), limit(a));
            return Object{{"items", json(list)}, {"nextOffset", list.size() == limit(a) ? Value(qint64(offset(a)) + list.size()) : Value()}};
        });
    add("delete_entity_region", "Stage deletion of every scanned entity in one 16 by 16 region using chunk coordinates. Save separately to persist.",
        "session:string dimension:integer x:integer z:integer", true, [this](const Object& a) {
            auto* w = editableWorld(app_, a);
            require(w->canDeleteEntityRegion(), "A fresh completed entity scan and an idle world are required.");
            const auto row = w->worldEntities()->findRegion({integer(a, "x"), integer(a, "z"), integer(a, "dimension")});
            require(row.has_value(), "No scanned entity region at those coordinates.");
            const auto id = startJob(w->mapController());
            auto* observer = jobs_.value(id).observer;
            connect(w->mapController(), &MapController::operationFinished, observer, [this, id] { finishJob(id); });
            connect(w->mapController(), &MapController::operationFailed, observer, [this, id](const QString& error) { finishJob(id, error); });
            if (!w->deleteEntityRegion(*row)) finishJob(id, "The region deletion could not start.");
            return jobs_.value(id).state;
        });
}

void McpServer::Tools::registerDocumentTools() {
    add("chunk_editor", "Open, inspect, apply or clear a chunk editor. x/z are chunk coordinates. Apply stages edits until the world is saved.",
        "session:string operation:string=open|clear|apply|terrain|export_statistic x:integer? z:integer? dimension:integer? y:integer? layer:integer? destination:string? row:integer? discard:boolean?",
        true, [this](const Object& a) {
            auto* w = world(app_, a);
            auto* c = w->chunkEditor();
            const auto action = string(a, "operation");
            if (action == "open") {
                require(!busy(w) && !c->exporting(), "Wait for the current operation to finish.");
                require(!c->dirty() || boolean(a, "discard"), "Apply the current chunk changes or pass discard=true.");
                require(a.contains("x") && a.contains("z") && a.contains("dimension"), "Supply x, z and dimension.");
                c->openChunk(regionCoordinate(a, "x"), regionCoordinate(a, "z"), integer(a, "dimension"));
                require(c->loading() || c->hasChunk(), "The chunk could not be opened.");
                app_.selectWorkspace(app_.openSessions()->indexOf(w)); w->setWorkspaceSection(0); w->setWorkspaceRoute("chunk-editor");
                return result("requested");
            }
            if (action == "clear") {
                require(!busy(w) && !c->exporting(), "Wait for the current operation to finish.");
                require(!c->dirty() || boolean(a, "discard"), "Apply the current chunk changes or pass discard=true.");
                c->clear(); return result();
            }
            require(c->hasChunk() && !c->loading(), "No chunk is ready.");
            if (action == "apply") {
                require(!busy(w), "Wait for the current operation to finish.");
                require(c->saveToCache(), "Could not apply the chunk changes."); return result();
            }
            if (action == "terrain") {
                if (a.contains("y")) c->setTerrainY(integer(a, "y"));
                if (a.contains("layer")) c->setTerrainLayer(integer(a, "layer"));
                return Object{{"cells", json(c->terrainCells())}, {"y", c->terrainY()}, {"layer", c->terrainLayer()}};
            }
            if (action == "export_statistic") {
                require(c->exportStatistic(integer(a, "row", -1), file(a, "destination")), "Could not export the raw chunk record."); return result();
            }
            throw ToolError{"Unknown chunk editor operation."};
        });
    add("get_chunk_editor", "Read status, coordinates, layer range and records of the current chunk editor.", "session:string", false,
        [this](const Object& a) {
            auto* c = world(app_, a)->chunkEditor();
            return Object{{"loading", c->loading()}, {"hasChunk", c->hasChunk()}, {"error", c->errorMessage()}, {"dirty", c->dirty()},
                          {"x", c->chunkX()}, {"z", c->chunkZ()}, {"dimension", c->dimension()},
                          {"minY", c->terrainMinY()}, {"maxY", c->terrainMaxY()}, {"maxLayer", c->terrainMaxLayer()},
                          {"statistics", json(c->statistics())}};
        });
    add("list_global_sections", "List the world's global NBT sections, such as level.dat, players, villages, maps and structures.", "session:string", false,
        [this](const Object& a) {
            const auto* global = world(app_, a)->globalData();
            Array items;
            const auto sections = global->sections();
            for (qsizetype i = 0; i < sections.size(); ++i) {
                auto* entry = qobject_cast<GlobalNbtSection*>(sections.at(i).value<QObject*>());
                if (entry) items.append(Object{{"section", int(i)}, {"title", entry->title()}, {"count", entry->count()},
                                               {"dirty", entry->isDirty()}, {"readOnly", entry->document()->readOnly()}});
            }
            return Object{{"loading", global->loading()}, {"loaded", global->loaded()}, {"error", global->error()}, {"items", items}};
        });
    add("list_nbt_records", "Read records in an NBT source: file, global (provide section), entities, block_entities, pending_ticks or structure.",
        "session:string source:string? section:integer? offset:integer? limit:integer?", false, [this](const Object& a) {
            const auto* doc = document(app_, a);
            const auto labels = doc->itemLabels();
            const int start = std::min(offset(a), doc->rootCount()), end = start + std::min(limit(a), doc->rootCount() - start);
            Array items;
            for (int i = start; i < end; ++i) items.append(Object{{"record", i}, {"label", labels.value(i)}});
            return Object{{"items", items}, {"total", doc->rootCount()}, {"current", doc->currentIndex()},
                          {"dirty", doc->dirty()}, {"readOnly", doc->readOnly()}, {"revision", doc->treeModel()->mcpRevision()},
                          {"nextOffset", end < doc->rootCount() ? Value(end) : Value()}};
        });
    add("read_nbt_tree", "Inspect visible NBT tree nodes; provide record to select a record first.",
        "session:string source:string? section:integer? record:integer? offset:integer? limit:integer?", false, [this](const Object& a) {
            auto* doc = document(app_, a);
            if (a.contains("record")) {
                const auto record = integer(a, "record");
                require(record >= 0 && record < doc->rootCount(), "Invalid NBT record index.");
                doc->selectItem(record);
            }
            auto out = page(*doc->treeModel(), a);
            out.insert("record", doc->currentIndex());
            out.insert("revision", doc->treeModel()->mcpRevision()); return out;
        });
    add("nbt_view", "Change NBT tree presentation: toggle a node, expand or collapse all, set a search string or show hex values.",
        "session:string source:string? section:integer? operation:string=toggle|expand_all|collapse_all|search|hex row:integer? search:string? enabled:boolean?", false,
        [this](const Object& a) {
            auto* tree = document(app_, a)->treeModel();
            const auto action = string(a, "operation");
            if (action == "toggle") require(tree->toggleExpanded(integer(a, "row", -1)), "Cannot expand the requested row.");
            else if (action == "expand_all") tree->expandAll();
            else if (action == "collapse_all") tree->collapseAll();
            else if (action == "search") tree->setSearchText(string(a, "search"));
            else if (action == "hex") tree->setHexMode(boolean(a, "enabled"));
            else throw ToolError{"Unknown NBT view operation."};
            return result();
        });
    add("edit_nbt", "Add/remove a record, clear a document, or update/add/remove/clear a visible node in a specific record.",
        "session:string source:string? section:integer? operation:string=add_record|remove_record|clear_document|update|add_child|remove_node|clear_children revision:string record:integer? row:integer? type:integer? key:string? value:string?", true,
        [this](const Object& a) {
            auto* doc = document(app_, a, true);
            auto* tree = doc->treeModel();
            const auto action = string(a, "operation");
            const auto record = integer(a, "record", -1), row = integer(a, "row", -1);
            require(tree->mcpRevision() == string(a, "revision"), "The NBT tree changed. Call read_nbt_tree.");
            if (action == "add_record") {
                const int added = doc->addItem(); require(added >= 0, doc->lastError());
                return Object{{"record", added}};
            }
            if (action == "remove_record") { require(doc->removeItem(record), doc->lastError()); return result(); }
            if (action == "clear_document") { require(doc->clearContents(), doc->lastError()); return result(); }
            require(record >= 0 && record == doc->currentIndex(), "The current record changed. Call read_nbt_tree first.");
            bool ok = false;
            if (action == "update") ok = tree->updateNode(row, string(a, "key"), string(a, "value"));
            else if (action == "add_child") ok = tree->addChild(row, integer(a, "type", -1), string(a, "key"), string(a, "value"));
            else if (action == "remove_node") ok = tree->removeNode(row);
            else if (action == "clear_children") ok = tree->clearChildren(row);
            else throw ToolError{"Unknown NBT edit operation."};
            require(ok, tree->lastError().isEmpty() ? QStringLiteral("The NBT edit was rejected.") : tree->lastError()); return result();
        });
    add("nbt_file_operation", "Import or replace records, export all or the current record, or save an NBT file under a new name.",
        "session:string source:string? section:integer? operation:string=import|replace|export|export_current|save_as path:string", true, [this](const Object& a) {
            const auto action = string(a, "operation");
            auto* doc = document(app_, a, action != "export" && action != "export_current");
            const auto url = file(a);
            bool ok = false;
            if (action == "import") ok = doc->importFile(url);
            else if (action == "replace") ok = doc->replaceFile(url);
            else if (action == "export") ok = doc->exportFile(url);
            else if (action == "export_current") ok = doc->exportCurrent(url);
            else if (action == "save_as") {
                require(qobject_cast<NbtSession*>(session(app_, a)), "Save a world or chunk document through its owning workspace.");
                ok = doc->saveAs(url);
            } else throw ToolError{"Unknown NBT file operation."};
            require(ok, doc->lastError()); return result();
        });
    add("map_item_operation", "Inspect, export, replace or lock the selected map image in a global NBT section.",
        "session:string section:integer operation:string=inspect|export|replace|lock path:string?", true, [this](const Object& a) {
            auto* controller = section(editableWorld(app_, a), a)->mapPreview();
            require(controller, "This global section does not have a map preview.");
            const auto action = string(a, "operation");
            if (action == "inspect") return Object{{"hasImage", controller->hasImage()}, {"locked", controller->locked()}};
            bool ok = false;
            if (action == "export") ok = controller->saveImage(file(a));
            else if (action == "replace") ok = controller->replaceImage(file(a));
            else if (action == "lock") ok = controller->lockMap();
            else throw ToolError{"Unknown map image operation."};
            require(ok, controller->lastError()); return result();
        });
    add("open_3d_area", "Prepare a 3D preview from an inclusive rectangle of world chunks.",
        "session:string dimension:integer minX:integer minZ:integer maxX:integer maxZ:integer", false, [this](const Object& a) {
            auto* w = editableWorld(app_, a);
            const auto minX = regionCoordinate(a, "minX"), minZ = regionCoordinate(a, "minZ");
            const auto maxX = regionCoordinate(a, "maxX"), maxZ = regionCoordinate(a, "maxZ");
            require(minX <= maxX && minZ <= maxZ, "Area limits are reversed.");
            auto* v = w->voxelView();
            require(!v->exporting() && !v->importPreviewActive(), "Finish the active export or import preview first.");
            require(!setting::current().limitVoxelPreviewArea || (maxX - minX < 16 && maxZ - minZ < 16),
                    "The 3D preview is limited to 16 by 16 chunks by the current setting.");
            v->openArea(minX, minZ, maxX, maxZ, integer(a, "dimension"));
            require(v->loading(), "The 3D preview could not start.");
            app_.selectWorkspace(app_.openSessions()->indexOf(w)); w->setWorkspaceSection(0); w->setWorkspaceRoute("voxel");
            return result("requested");
        });
    add("get_3d_preview", "Read a 3D preview's local size, world origin, import size and loading status. Source: world, chunk, global (provide section), or structure.",
        "session:string source:string? section:integer?", false, [this](const Object& a) {
            auto* v = voxel(app_, a);
            const auto scene = v->voxelScene();
            Object out{{"loading", v->loading()}, {"progress", v->voxelProgress()}, {"error", v->errorMessage()},
                       {"deleting", v->deleting()}, {"exporting", v->exporting()}, {"hasModel", v->hasVoxelModel()}};
            if (scene) { out.insert("size", vector(scene->modelSize)); out.insert("origin", vector(scene->origin)); }
            if (auto* b = qobject_cast<BlockVolumeController*>(v)) {
                out.insert("importing", b->importing()); out.insert("copying", b->copying());
                out.insert("importPreviewActive", b->importPreviewActive()); out.insert("importSize", vector(b->importSize()));
            }
            return out;
        });
    add("export_3d_selection", "Export local block bounds [min,max) to GLB or mcstructure.",
        "session:string source:string? section:integer? format:string=glb|mcstructure destination:string min:array max:array entities:boolean? newFormat:boolean?", true,
        [this](const Object& a) {
            auto* v = voxel(app_, a);
            const auto url = file(a, "destination");
            const auto minimum = vector(a, "min"), maximum = vector(a, "max");
            const auto format = string(a, "format");
            require(!v->exporting() && (format == "glb" || format == "mcstructure"), "Invalid format or an export is already running.");
            const auto id = startJob(v);
            connect(v, &VoxelSceneController::exportFinished, jobs_.value(id).observer,
                    [this, id](const QUrl&, const QString& error) { finishJob(id, error); });
            const auto ok = format == "glb" ? v->exportGlb(url, minimum, maximum) :
                v->exportMcstructure(url, minimum, maximum, boolean(a, "entities", true), boolean(a, "newFormat", true));
            if (!ok) finishJob(id, "The 3D export was rejected.");
            return jobs_.value(id).state;
        });
    add("edit_3d_selection", "Copy, import, paste, apply_import, cancel_import or delete a 3D selection. Bounds are local blocks, [min,max).",
        "session:string source:string? section:integer? operation:string=copy|import|paste|apply_import|cancel_import|delete min:array? max:array? path:string? entities:boolean? newFormat:boolean?", true,
        [this](const Object& a) {
            if (qobject_cast<WorldSession*>(session(app_, a)))
                require(!editableWorld(app_, a)->chunkEditor()->dirty(), "Apply the current chunk edits first.");
            auto* v = volume(app_, a);
            const auto action = string(a, "operation");
            if (action == "cancel_import") { v->cancelImport(); return result(); }
            if (action == "import" || action == "paste") {
                const bool ok = action == "import" ? v->importStructure(file(a)) : v->pasteStructure();
                require(ok, "The structure preview could not start."); return result("requested");
            }
            const auto minimum = vector(a, "min"), maximum = vector(a, "max");
            require(!v->loading() && !v->copying() && !v->importing() && !v->deleting() && !v->exporting(), "Wait for the current 3D operation.");
            require(action == "copy" || action == "apply_import" || action == "delete", "Unknown 3D operation.");
            const auto id = startJob(v);
            auto* observer = jobs_.value(id).observer;
            bool ok = false;
            if (action == "copy") {
                connect(v, &BlockVolumeController::copyFinished, observer, [this, id] { finishJob(id); });
                connect(v, &BlockVolumeController::copyFailed, observer, [this, id](const QString& error) { finishJob(id, error); });
                ok = v->copySelection(minimum, maximum, boolean(a, "entities", true), boolean(a, "newFormat", true));
            } else if (action == "apply_import") {
                connect(v, &BlockVolumeController::importFinished, observer, [this, id] { finishJob(id); });
                connect(v, &BlockVolumeController::importFailed, observer, [this, id](const QString& error) { finishJob(id, error); });
                ok = v->commitImport(minimum, maximum, boolean(a, "entities", true));
            } else {
                connect(v, &BlockVolumeController::regionsChanged, observer, [this, id] { finishJob(id); });
                connect(v, &BlockVolumeController::deleteFailed, observer, [this, id](const QString& error) { finishJob(id, error); });
                ok = v->deleteBlockVolumeAsync(minimum, maximum, boolean(a, "entities"));
            }
            if (!ok) finishJob(id, "The 3D operation was rejected.");
            return jobs_.value(id).state;
        });
}
