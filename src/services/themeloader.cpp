#include "services/themeloader.h"
#define TOML_HEADER_ONLY 1
#include "third_party/toml.hpp"
#include <QFile>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QDebug>

QMap<QString, QColor> ThemeLoader::s_defaults = {
    {"base", QColor("#1e1e2e")}, {"mantle", QColor("#181825")},
    {"crust", QColor("#11111b")}, {"surface", QColor("#313244")},
    {"overlay", QColor("#45475a")}, {"text", QColor("#cdd6f4")},
    {"subtext", QColor("#bac2de")}, {"muted", QColor("#6c7086")},
    {"accent", QColor("#89b4fa")}, {"success", QColor("#a6e3a1")},
    {"warning", QColor("#f9e2af")}, {"error", QColor("#f38ba8")},
};

ThemeLoader::ThemeLoader(QObject *parent) : QObject(parent), m_colors(s_defaults)
{
    // Same trick ConfigManager uses: editors save by writing a temp file and
    // renaming over the target, which gives the theme a new inode and drops a
    // file-only watch. Watch the containing directory too and re-arm from
    // either signal.
    const auto rearmAndReload = [this]() {
        if (m_watchedPath.isEmpty())
            return;
        const QFileInfo info(m_watchedPath);
        if (!info.exists())
            return;
        if (!m_watcher.files().contains(m_watchedPath))
            m_watcher.addPath(m_watchedPath);
        // The directory signal also fires for sibling themes, so only reload
        // when the theme actually in use moved on.
        if (info.lastModified() == m_watchedModified)
            return;
        m_watchedModified = info.lastModified();
        applyThemeFile(m_watchedPath);
    };
    connect(&m_watcher, &QFileSystemWatcher::fileChanged, this, rearmAndReload);
    connect(&m_watcher, &QFileSystemWatcher::directoryChanged, this, rearmAndReload);
}

void ThemeLoader::watchThemeFile(const QString &filePath)
{
    if (filePath == m_watchedPath)
        return;
    if (!m_watcher.files().isEmpty())
        m_watcher.removePaths(m_watcher.files());
    if (!m_watcher.directories().isEmpty())
        m_watcher.removePaths(m_watcher.directories());
    m_watchedPath = filePath;
    m_watchedModified = QFileInfo(filePath).lastModified();
    m_watcher.addPath(filePath);
    m_watcher.addPath(QFileInfo(filePath).absolutePath());
}

void ThemeLoader::applyThemeFile(const QString &filePath)
{
    if (filePath.endsWith(QStringLiteral(".json"))) {
        applyPywalFile(filePath);
        return;
    }
    m_colors = s_defaults;
    try {
        auto config = toml::parse_file(filePath.toStdString());
        if (auto colors = config["colors"].as_table()) {
            for (const auto &[key, val] : *colors) {
                if (auto v = val.value<std::string>()) {
                    QString colorStr = QString::fromStdString(*v);
                    QColor c(colorStr);
                    if (c.isValid())
                        m_colors[QString::fromStdString(std::string(key))] = c;
                }
            }
        }
    } catch (const toml::parse_error &err) {
        qWarning() << "Theme parse error:" << err.what();
    }
    emit themeChanged();
}

// pywal only gives a background, a foreground and sixteen terminal colours, so
// the surface ramp is mixed from background towards foreground. That keeps it
// right for both dark and light (wal -l) palettes. Mantle and crust are darker
// than base in both, like the bundled themes.
void ThemeLoader::applyPywalFile(const QString &filePath)
{
    m_colors = s_defaults;
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) {
        qWarning() << "Theme not readable:" << filePath;
        emit themeChanged();
        return;
    }
    const QJsonObject root = QJsonDocument::fromJson(file.readAll()).object();
    const QJsonObject special = root.value("special").toObject();
    const QJsonObject colors = root.value("colors").toObject();
    const QColor bg(special.value("background").toString());
    const QColor fg(special.value("foreground").toString());
    if (!bg.isValid() || !fg.isValid()) {
        qWarning() << "Theme parse error: no pywal background/foreground in" << filePath;
        emit themeChanged();
        return;
    }
    const auto mix = [&](qreal t) {
        return QColor::fromRgbF(bg.redF() + (fg.redF() - bg.redF()) * t,
                                bg.greenF() + (fg.greenF() - bg.greenF()) * t,
                                bg.blueF() + (fg.blueF() - bg.blueF()) * t);
    };
    m_colors["base"] = bg;
    m_colors["mantle"] = bg.darker(108);
    m_colors["crust"] = bg.darker(116);
    m_colors["surface"] = mix(0.12);
    m_colors["overlay"] = mix(0.24);
    m_colors["muted"] = mix(0.45);
    m_colors["subtext"] = mix(0.8);
    m_colors["text"] = fg;
    for (const auto &[key, index] : {std::pair{"accent", "color4"}, std::pair{"success", "color2"},
                                     std::pair{"warning", "color3"}, std::pair{"error", "color1"}}) {
        const QColor c(colors.value(index).toString());
        if (c.isValid())
            m_colors[key] = c;
    }
    emit themeChanged();
}

void ThemeLoader::loadTheme(const QString &nameOrPath, const QStringList &themesDirs)
{
    QString filePath;
    if (nameOrPath == QStringLiteral("pywal")) {
        filePath = pywalColorsPath();
    } else if (QFile::exists(nameOrPath)) {
        filePath = nameOrPath;
    } else {
        // First directory wins, so the user's ~/.config themes shadow bundled ones.
        for (const QString &dir : themesDirs) {
            if (dir.isEmpty())
                continue;
            const QString candidate = QDir(dir).filePath(nameOrPath + ".toml");
            if (QFile::exists(candidate)) {
                filePath = candidate;
                break;
            }
        }
    }
    if (filePath.isEmpty() || !QFile::exists(filePath)) {
        qWarning() << "Theme not found:" << nameOrPath;
        m_colors = s_defaults;
        emit themeChanged();
        return;
    }
    watchThemeFile(QFileInfo(filePath).absoluteFilePath());
    applyThemeFile(filePath);
}

QColor ThemeLoader::color(const QString &name) const
{
    return m_colors.value(name, s_defaults.value(name, QColor("#ff00ff")));
}
