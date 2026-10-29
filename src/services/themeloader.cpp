#include "services/themeloader.h"
#define TOML_HEADER_ONLY 1
#include "third_party/toml.hpp"
#include <QFile>
#include <QDir>
#include <QFileInfo>
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
    m_colors = s_defaults;
    m_terminalPalette.clear();
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
        // All 16 or none: half a palette mixed with the defaults would clash.
        if (auto palette = config["terminal"]["palette"].as_array(); palette && palette->size() == 16) {
            QVariantList colors;
            for (const auto &entry : *palette) {
                const QColor c(QString::fromStdString(entry.value_or(std::string())));
                if (!c.isValid())
                    break;
                colors.append(c);
            }
            if (colors.size() == 16)
                m_terminalPalette = colors;
            else
                qWarning() << "Theme" << filePath << "has an invalid terminal palette";
        }
    } catch (const toml::parse_error &err) {
        qWarning() << "Theme parse error:" << err.what();
    }
    emit themeChanged();
}

void ThemeLoader::loadTheme(const QString &nameOrPath, const QStringList &themesDirs)
{
    QString filePath;
    if (QFile::exists(nameOrPath)) {
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
        m_terminalPalette.clear();
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
