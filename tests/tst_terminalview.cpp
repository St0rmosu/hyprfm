#include <QTest>
#include <QFileInfo>
#include <QImage>
#include <QKeyEvent>
#include <QPainter>
#include <QSignalSpy>
#include <QTemporaryDir>

#include "terminal/terminalview.h"

// The built-in terminal against a real /bin/sh in a pty (not the user's
// shell, whose startup files would make timing and output unpredictable).
class TestTerminalView : public QObject
{
    Q_OBJECT

    static void type(TerminalView &view, const QString &text)
    {
        for (const QChar c : text) {
            const int key = c == QLatin1Char('\n') ? int(Qt::Key_Return) : int(c.toUpper().unicode());
            const QString keyText = c == QLatin1Char('\n') ? QStringLiteral("\r") : QString(c);
            QKeyEvent press(QEvent::KeyPress, key, Qt::NoModifier, keyText);
            QCoreApplication::sendEvent(&view, &press);
            QKeyEvent release(QEvent::KeyRelease, key, Qt::NoModifier, keyText);
            QCoreApplication::sendEvent(&view, &release);
        }
    }

private slots:
    void initTestCase()
    {
        qputenv("SHELL", "/bin/sh");
        qputenv("ENV", "");   // no ~/.shrc for an interactive sh
    }

    // Keys reach the shell through Ghostty's encoder, in the starting folder.
    void testTypedCommandRuns()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        TerminalView view;
        view.setSize(QSizeF(640, 240));
        view.setWorkingDirectory(dir.path());
        view.start();
        QVERIFY(view.running());

        type(view, QStringLiteral("touch typed-here\n"));
        QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(dir.filePath("typed-here")), 5000);
    }

    // OSC 7 from the shell sets shellDirectory.
    void testShellReportsItsFolder()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        TerminalView view;
        view.setSize(QSizeF(640, 240));
        view.setWorkingDirectory(dir.path());
        QSignalSpy changed(&view, &TerminalView::shellDirectoryChanged);
        view.start();
        view.sendText(QStringLiteral("printf '\\033]7;file://localhost%s\\007' \"$PWD\"\n"));
        QTRY_VERIFY_WITH_TIMEOUT(changed.count() > 0, 5000);
        QCOMPARE(QFileInfo(view.shellDirectory()).canonicalFilePath(), QFileInfo(dir.path()).canonicalFilePath());
    }

    // What the shell prints is painted: a line of X in red shows up in the image.
    void testOutputIsPainted()
    {
        TerminalView view;
        view.setSize(QSizeF(640, 240));
        view.setBackground(Qt::black);
        view.start();
        view.sendText(QStringLiteral("clear; printf '\\033[31mXXXXXXXXXXXXXXXXXXXX\\033[0m\\n'\n"));

        const auto redPixels = [&] {
            QImage image(640, 240, QImage::Format_RGB32);
            QPainter painter(&image);
            view.paint(&painter);
            painter.end();
            int red = 0;
            for (int y = 0; y < image.height(); ++y) {
                for (int x = 0; x < image.width(); ++x) {
                    const QColor c = image.pixelColor(x, y);
                    if (c.red() > 150 && c.green() < 100 && c.blue() < 100)
                        ++red;
                }
            }
            return red;
        };
        QTRY_VERIFY_WITH_TIMEOUT(redPixels() > 50, 5000);
    }

    void testExitEndsTheSession()
    {
        TerminalView view;
        view.setSize(QSizeF(640, 240));
        QSignalSpy finished(&view, &TerminalView::finished);
        view.start();
        view.sendText(QStringLiteral("exit\n"));
        QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 5000);
        QVERIFY(!view.running());
    }
};

QTEST_MAIN(TestTerminalView)
#include "tst_terminalview.moc"
