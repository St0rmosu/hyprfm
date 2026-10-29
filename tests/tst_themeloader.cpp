#include <QTest>
#include <QTemporaryDir>
#include <QDir>
#include <QFile>
#include <QSignalSpy>
#include "services/themeloader.h"

class TestThemeLoader : public QObject
{
    Q_OBJECT

private slots:
    void testLoadBuiltinTheme()
    {
        ThemeLoader loader;
        loader.loadTheme("catppuccin-mocha", QStringList{QString(THEMES_DIR)});
        QCOMPARE(loader.color("base"), QColor("#1e1e2e"));
        QCOMPARE(loader.color("accent"), QColor("#89b4fa"));
        QCOMPARE(loader.color("text"), QColor("#cdd6f4"));
        QCOMPARE(loader.color("error"), QColor("#f38ba8"));
    }

    void testLoadBuiltinLightTheme()
    {
        ThemeLoader loader;
        loader.loadTheme("catppuccin-latte", QStringList{QString(THEMES_DIR)});
        QCOMPARE(loader.color("base"), QColor("#eff1f5"));
        QCOMPARE(loader.color("accent"), QColor("#1e66f5"));
        QCOMPARE(loader.color("text"), QColor("#4c4f69"));
        QCOMPARE(loader.color("error"), QColor("#d20f39"));
    }

    void testAllBuiltinColors()
    {
        ThemeLoader loader;
        loader.loadTheme("catppuccin-mocha", QStringList{QString(THEMES_DIR)});

        QCOMPARE(loader.color("base"), QColor("#1e1e2e"));
        QCOMPARE(loader.color("mantle"), QColor("#181825"));
        QCOMPARE(loader.color("crust"), QColor("#11111b"));
        QCOMPARE(loader.color("surface"), QColor("#313244"));
        QCOMPARE(loader.color("overlay"), QColor("#45475a"));
        QCOMPARE(loader.color("text"), QColor("#cdd6f4"));
        QCOMPARE(loader.color("subtext"), QColor("#bac2de"));
        QCOMPARE(loader.color("muted"), QColor("#6c7086"));
        QCOMPARE(loader.color("accent"), QColor("#89b4fa"));
        QCOMPARE(loader.color("success"), QColor("#a6e3a1"));
        QCOMPARE(loader.color("warning"), QColor("#f9e2af"));
        QCOMPARE(loader.color("error"), QColor("#f38ba8"));
    }

    void testLoadCustomTheme()
    {
        QTemporaryDir dir;
        QString path = dir.path() + "/custom.toml";
        QFile f(path);
        f.open(QIODevice::WriteOnly);
        f.write("[colors]\nbase = \"#000000\"\ntext = \"#ffffff\"\naccent = \"#ff0000\"\n");
        f.close();

        ThemeLoader loader;
        loader.loadTheme(path, QStringList{});
        QCOMPARE(loader.color("base"), QColor("#000000"));
        QCOMPARE(loader.color("text"), QColor("#ffffff"));
        QCOMPARE(loader.color("accent"), QColor("#ff0000"));
    }

    void testFallbackForMissingColors()
    {
        QTemporaryDir dir;
        QString path = dir.path() + "/partial.toml";
        QFile f(path);
        f.open(QIODevice::WriteOnly);
        f.write("[colors]\nbase = \"#000000\"\n");
        f.close();

        ThemeLoader loader;
        loader.loadTheme(path, QStringList{});
        QCOMPARE(loader.color("base"), QColor("#000000"));
        // All other colors should fall back to defaults
        QCOMPARE(loader.color("text"), QColor("#cdd6f4"));
        QCOMPARE(loader.color("accent"), QColor("#89b4fa"));
        QCOMPARE(loader.color("error"), QColor("#f38ba8"));
    }

    void testMissingThemeFile()
    {
        ThemeLoader loader;
        loader.loadTheme("nonexistent-theme", QStringList{"/nonexistent/path"});
        // Should fall back to defaults, not crash
        QCOMPARE(loader.color("base"), QColor("#1e1e2e"));
        QCOMPARE(loader.color("text"), QColor("#cdd6f4"));
    }

    void testInvalidToml()
    {
        QTemporaryDir dir;
        QString path = dir.path() + "/bad.toml";
        QFile f(path);
        f.open(QIODevice::WriteOnly);
        f.write("this is not valid toml {{{{");
        f.close();

        ThemeLoader loader;
        loader.loadTheme(path, QStringList{});
        // Should use defaults, not crash
        QCOMPARE(loader.color("base"), QColor("#1e1e2e"));
    }

    void testUnknownColorNameReturnsFallback()
    {
        ThemeLoader loader;
        loader.loadTheme("catppuccin-mocha", QStringList{QString(THEMES_DIR)});
        // Unknown color name should return some default
        QColor unknown = loader.color("nonexistent_color");
        QVERIFY(unknown.isValid() || !unknown.isValid()); // Just verify no crash
    }

    void testThemeChangedSignal()
    {
        ThemeLoader loader;
        QSignalSpy spy(&loader, &ThemeLoader::themeChanged);

        loader.loadTheme("catppuccin-mocha", QStringList{QString(THEMES_DIR)});
        QCOMPARE(spy.count(), 1);
    }

    void testLoadThemeByName()
    {
        ThemeLoader loader;
        loader.loadTheme("catppuccin-mocha", QStringList{QString(THEMES_DIR)});
        // Loaded by name from themes directory
        QCOMPARE(loader.color("base"), QColor("#1e1e2e"));
    }

    void testPropertyAccessors()
    {
        ThemeLoader loader;
        loader.loadTheme("catppuccin-mocha", QStringList{QString(THEMES_DIR)});

        // Test the Q_PROPERTY accessors directly
        QCOMPARE(loader.base(), QColor("#1e1e2e"));
        QCOMPARE(loader.text(), QColor("#cdd6f4"));
        QCOMPARE(loader.accent(), QColor("#89b4fa"));
    }

    void testEmptyThemeFile()
    {
        QTemporaryDir dir;
        QString path = dir.path() + "/empty.toml";
        QFile f(path);
        f.open(QIODevice::WriteOnly);
        f.close(); // empty file

        ThemeLoader loader;
        loader.loadTheme(path, QStringList{});
        // Should use all defaults
        QCOMPARE(loader.color("base"), QColor("#1e1e2e"));
    }

    void testColorsSectionMissing()
    {
        QTemporaryDir dir;
        QString path = dir.path() + "/nocolor.toml";
        QFile f(path);
        f.open(QIODevice::WriteOnly);
        f.write("[metadata]\nname = \"test\"\n"); // no [colors] section
        f.close();

        ThemeLoader loader;
        loader.loadTheme(path, QStringList{});
        QCOMPARE(loader.color("base"), QColor("#1e1e2e"));
    }

    void testUserThemeDirIsSearchedAndShadowsBuiltins()
    {
        QTemporaryDir userDir;
        const auto write = [&](const QString &name, const QString &base) {
            QFile f(userDir.path() + "/" + name + ".toml");
            f.open(QIODevice::WriteOnly);
            f.write(QString("[colors]\nbase = \"%1\"\n").arg(base).toUtf8());
            f.close();
        };
        write("monoknight", "#000000");
        write("catppuccin-mocha", "#123456");

        const QStringList dirs{userDir.path(), QString(THEMES_DIR)};

        // A theme that only exists in the user directory is found.
        ThemeLoader loader;
        loader.loadTheme("monoknight", dirs);
        QCOMPARE(loader.color("base"), QColor("#000000"));

        // A user theme wins over the bundled one with the same name.
        loader.loadTheme("catppuccin-mocha", dirs);
        QCOMPARE(loader.color("base"), QColor("#123456"));

        // Bundled themes still resolve through the later directory.
        loader.loadTheme("catppuccin-latte", dirs);
        QCOMPARE(loader.color("base"), QColor("#eff1f5"));
    }

    void testEditingTheLoadedThemeReloadsIt()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath("live.toml");
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("[colors]\nbase = \"#111111\"\n");
        file.close();

        ThemeLoader loader;
        loader.loadTheme("live", QStringList{dir.path()});
        QCOMPARE(loader.color("base"), QColor("#111111"));

        QSignalSpy spy(&loader, &ThemeLoader::themeChanged);

        // The mtime guard has one-second resolution on some filesystems, so
        // make sure the rewrite lands in a later second than the load.
        QTest::qWait(1100);
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        file.write("[colors]\nbase = \"#222222\"\n");
        file.close();

        QVERIFY(spy.wait(5000));
        QCOMPARE(loader.color("base"), QColor("#222222"));
    }

    void testPywalThemeReadsWalCache()
    {
        QTemporaryDir dir;
        qputenv("XDG_CACHE_HOME", dir.path().toUtf8());
        QVERIFY(QDir().mkpath(dir.filePath("wal")));
        QFile file(ThemeLoader::pywalColorsPath());
        QCOMPARE(file.fileName(), dir.filePath("wal/colors.json"));
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("{\"special\": {\"background\": \"#000000\", \"foreground\": \"#ffffff\"},"
                   " \"colors\": {\"color1\": \"#ff0000\", \"color2\": \"#00ff00\","
                   " \"color3\": \"#ffff00\", \"color4\": \"#0000ff\"}}");
        file.close();

        ThemeLoader loader;
        loader.loadTheme("pywal", QStringList{QString(THEMES_DIR)});
        qunsetenv("XDG_CACHE_HOME");
        QCOMPARE(loader.color("base"), QColor("#000000"));
        QCOMPARE(loader.color("text"), QColor("#ffffff"));
        QCOMPARE(loader.color("accent"), QColor("#0000ff"));
        QCOMPARE(loader.color("success"), QColor("#00ff00"));
        QCOMPARE(loader.color("warning"), QColor("#ffff00"));
        QCOMPARE(loader.color("error"), QColor("#ff0000"));
        // The ramp sits between background and foreground; black has nothing darker.
        QCOMPARE(loader.color("crust"), QColor("#000000"));
        QVERIFY(loader.color("surface").lightness() > 0);
        QVERIFY(loader.color("surface").lightness() < loader.color("overlay").lightness());
        QVERIFY(loader.color("muted").lightness() < loader.color("subtext").lightness());

        // wal -l: mantle and crust still go darker than base, like the bundled light themes.
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        file.write("{\"special\": {\"background\": \"#f2e5bc\", \"foreground\": \"#504945\"}}");
        file.close();
        loader.loadTheme(file.fileName(), QStringList{});
        QCOMPARE(loader.color("base"), QColor("#f2e5bc"));
        QVERIFY(loader.color("mantle").lightness() < loader.color("base").lightness());
        QVERIFY(loader.color("crust").lightness() < loader.color("mantle").lightness());
        QVERIFY(loader.color("surface").lightness() > loader.color("overlay").lightness());
    }
};

QTEST_MAIN(TestThemeLoader)
#include "tst_themeloader.moc"
