#include <QTest>
#include <QFile>
#include <QWheelEvent>
#include <QClipboard>
#include <QFontMetricsF>
#include <QGuiApplication>
#include <QMouseEvent>
#include <QDir>
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

    // Centre of a cell, matching TerminalView's 6 px padding and whole-pixel cells.
    static QPointF cellCentre(const TerminalView &view, int column, int row)
    {
        const QFontMetricsF metrics(view.font());
        const qreal w = qCeil(metrics.horizontalAdvance(QLatin1Char('M')));
        const qreal h = qCeil(metrics.height());
        return QPointF(6 + column * w + w / 2, 6 + row * h + h / 2);
    }

    static void mouse(TerminalView &view, QEvent::Type type, QPointF pos, ulong time,
                      Qt::MouseButtons buttons = Qt::LeftButton)
    {
        QMouseEvent event(type, pos, view.mapToScene(pos), view.mapToScene(pos), Qt::LeftButton,
                          buttons, Qt::NoModifier);
        event.setTimestamp(time);
        QCoreApplication::sendEvent(&view, &event);
    }

    // A view with "hello world" on its top row and nothing else.
    static void showHelloWorld(TerminalView &view)
    {
        view.setSize(QSizeF(640, 240));
        view.start();
        view.sendText(QStringLiteral("clear; printf 'hello world'; sleep 5\n"));
        QTest::qWait(400);
        QImage image(640, 240, QImage::Format_RGB32);
        QPainter painter(&image);
        view.paint(&painter);   // folds the output into the render state
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

    // A grapheme longer than any fixed buffer (one base letter and 40
    // combining marks) paints without overrunning anything; ASan catches a
    // regression, a plain build at least runs the path.
    void testLongGraphemeIsPainted()
    {
        TerminalView view;
        view.setSize(QSizeF(640, 240));
        view.start();
        view.sendText(QStringLiteral("printf 'a'; for i in $(seq 40); do printf '\\314\\201'; done; printf '\\n'\n"));
        QImage image(640, 240, QImage::Format_RGB32);
        QTest::qWait(300);
        QPainter painter(&image);
        view.paint(&painter);
        QVERIFY(view.running());
    }

    // changeDirectory types a cd at the prompt, even into a quoted name, and
    // leaves a running program alone.
    void testChangeDirectory()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString target = dir.filePath(QStringLiteral("it's here"));
        QVERIFY(QDir().mkpath(target));
        TerminalView view;
        view.setSize(QSizeF(640, 240));
        view.start();
        QTRY_VERIFY_WITH_TIMEOUT(view.changeDirectory(target), 5000);
        view.sendText(QStringLiteral("touch arrived\n"));
        QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(target + QStringLiteral("/arrived")), 5000);

        // A control character would act as a key in the line editor.
        QVERIFY(!view.changeDirectory(dir.path() + QStringLiteral("/x\x03\rtouch pwned")));

        view.sendText(QStringLiteral("sleep 5\n"));
        QTRY_VERIFY_WITH_TIMEOUT(!view.changeDirectory(dir.path()), 5000);
    }

    // Dragging across cells selects them; Ctrl+Shift+C copies the text.
    void testDragSelectsAndCopies()
    {
        TerminalView view;
        showHelloWorld(view);
        mouse(view, QEvent::MouseButtonPress, cellCentre(view, 0, 0), 1000);
        // A cell joins the selection once the pointer passes its middle.
        const QPointF end = (cellCentre(view, 4, 0) + cellCentre(view, 5, 0)) / 2 - QPointF(1, 0);
        mouse(view, QEvent::MouseMove, end, 1010);
        mouse(view, QEvent::MouseButtonRelease, end, 1020, Qt::NoButton);
        QCOMPARE(view.selectedText(), QStringLiteral("hello"));

        QGuiApplication::clipboard()->clear();
        QKeyEvent copy(QEvent::KeyPress, Qt::Key_C, Qt::ControlModifier | Qt::ShiftModifier);
        QCoreApplication::sendEvent(&view, &copy);
        QCOMPARE(QGuiApplication::clipboard()->text(), QStringLiteral("hello"));
    }

    // A double click selects the word under it; a plain click clears it.
    void testDoubleClickSelectsWord()
    {
        TerminalView view;
        showHelloWorld(view);
        const QPointF w = cellCentre(view, 7, 0);
        mouse(view, QEvent::MouseButtonPress, w, 2000);
        mouse(view, QEvent::MouseButtonRelease, w, 2010, Qt::NoButton);
        mouse(view, QEvent::MouseButtonPress, w, 2050);
        mouse(view, QEvent::MouseButtonRelease, w, 2060, Qt::NoButton);
        QCOMPARE(view.selectedText(), QStringLiteral("world"));

        mouse(view, QEvent::MouseButtonPress, cellCentre(view, 2, 0), 9000);
        mouse(view, QEvent::MouseButtonRelease, cellCentre(view, 2, 0), 9010, Qt::NoButton);
        QCOMPARE(view.selectedText(), QString());
    }

    // Ctrl+Shift+V types the clipboard; escape bytes in it are defused, so
    // a paste cannot smuggle a terminal sequence in.
    void testPasteTypesTheClipboard()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        TerminalView view;
        view.setSize(QSizeF(640, 240));
        view.setWorkingDirectory(dir.path());
        view.start();
        QGuiApplication::clipboard()->setText(QStringLiteral("touch pasted\x1b[201~x"));
        QKeyEvent paste(QEvent::KeyPress, Qt::Key_V, Qt::ControlModifier | Qt::ShiftModifier);
        QCoreApplication::sendEvent(&view, &paste);
        type(view, QStringLiteral("\n"));
        QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(dir.filePath("pasted")), 5000);
        QVERIFY(QFileInfo::exists(dir.filePath("[201~x")));
    }

    // A program that turned on mouse tracking gets clicks, encoded in the
    // format it asked for (SGR here), instead of a selection.
    void testClickIsReportedToTrackingProgram()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        TerminalView view;
        view.setSize(QSizeF(640, 240));
        view.setWorkingDirectory(dir.path());
        view.start();
        view.sendText(QStringLiteral("printf '\\033[?1000h\\033[?1006h'; stty raw -echo; "
                                     "dd bs=1 count=9 of=clicks 2>/dev/null; stty sane\n"));
        QTest::qWait(400);
        QImage image(640, 240, QImage::Format_RGB32);
        QPainter painter(&image);
        view.paint(&painter);
        mouse(view, QEvent::MouseButtonPress, cellCentre(view, 2, 1), 1000);
        mouse(view, QEvent::MouseButtonRelease, cellCentre(view, 2, 1), 1010, Qt::NoButton);
        QTRY_COMPARE_WITH_TIMEOUT(QFileInfo(dir.filePath("clicks")).size(), 9, 5000);
        QFile clicks(dir.filePath("clicks"));
        QVERIFY(clicks.open(QIODevice::ReadOnly));
        QCOMPARE(clicks.readAll(), QByteArray("\x1b[<0;3;2M"));
        QCOMPARE(view.selectedText(), QString());
    }

    // less and man run on the alternate screen without mouse support; the
    // wheel scrolls them with arrow keys.
    void testWheelOnAlternateScreenSendsArrows()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        TerminalView view;
        view.setSize(QSizeF(640, 240));
        view.setWorkingDirectory(dir.path());
        view.start();
        view.sendText(QStringLiteral("printf '\\033[?1049h'; stty raw -echo; "
                                     "dd bs=1 count=3 of=keys 2>/dev/null; stty sane\n"));
        QTest::qWait(400);
        QImage image(640, 240, QImage::Format_RGB32);
        QPainter painter(&image);
        view.paint(&painter);
        const QPointF pos = cellCentre(view, 2, 1);
        QWheelEvent wheel(pos, view.mapToScene(pos), QPoint(), QPoint(0, 40), Qt::NoButton, Qt::NoModifier,
                          Qt::NoScrollPhase, false);
        QCoreApplication::sendEvent(&view, &wheel);
        QTRY_COMPARE_WITH_TIMEOUT(QFileInfo(dir.filePath("keys")).size(), 3, 5000);
        QFile keys(dir.filePath("keys"));
        QVERIFY(keys.open(QIODevice::ReadOnly));
        QCOMPARE(keys.readAll(), QByteArray("\x1b[A"));
    }

    // With bracketed paste off (bash turns it off while a command runs), a
    // multi-line paste would be typed line by line into whatever reads it;
    // it is held back for confirmation instead.
    void testUnbracketedMultiLinePasteAsksFirst()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        TerminalView view;
        view.setSize(QSizeF(640, 240));
        view.setWorkingDirectory(dir.path());
        view.start();
        view.sendText(QStringLiteral("cat > pasted.txt\n"));
        QTest::qWait(300);
        QImage image(640, 240, QImage::Format_RGB32);
        QPainter painter(&image);
        view.paint(&painter);
        QSignalSpy asked(&view, &TerminalView::unsafePaste);
        QGuiApplication::clipboard()->setText(QStringLiteral("touch first\ntouch second\n"));
        view.paste();
        QCOMPARE(asked.count(), 1);
        QTest::qWait(300);
        QCOMPARE(QFileInfo(dir.filePath("pasted.txt")).size(), 0);

        view.pasteText(asked.at(0).at(0).toString());
        QTRY_COMPARE_WITH_TIMEOUT(QFileInfo(dir.filePath("pasted.txt")).size(), 25, 5000);
    }

    // Runs of cells are drawn as one string; a 100-cell run of "=" (which
    // ligature fonts like to join) must still end inside cell 100.
    void testRunsStayOnTheGrid()
    {
        TerminalView view;
        view.setSize(QSizeF(1600, 240));
        view.setBackground(Qt::black);
        view.start();
        view.sendText(QStringLiteral("clear; printf '\\033[31m%s\\033[0m' \"$(printf '=%.0s' $(seq 100))\"; sleep 5\n"));
        const QFontMetricsF metrics(view.font());
        const qreal cell = qCeil(metrics.horizontalAdvance(QLatin1Char('M')));
        int rightmost = -1;
        QTRY_VERIFY_WITH_TIMEOUT([&] {
            QImage image(1600, 240, QImage::Format_RGB32);
            QPainter painter(&image);
            view.paint(&painter);
            painter.end();
            rightmost = -1;
            for (int y = 0; y < 60; ++y)
                for (int x = 0; x < image.width(); ++x) {
                    const QColor c = image.pixelColor(x, y);
                    if (c.red() > 150 && c.green() < 100 && x > rightmost)
                        rightmost = x;
                }
            return rightmost > 6 + 98 * cell;
        }(), 5000);
        QVERIFY2(rightmost < 6 + 100 * cell + 1, qPrintable(QString::number(rightmost)));
    }

    // A translucent background (window transparency on) stays translucent,
    // so the compositor's blur shows through behind the shell.
    void testTranslucentBackgroundKeepsItsAlpha()
    {
        TerminalView view;
        view.setSize(QSizeF(320, 120));
        view.setBackground(QColor(0x1e, 0x1e, 0x2e, 128));
        QVERIFY(!view.opaquePainting());
        QImage image(320, 120, QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::white);
        QPainter painter(&image);
        view.paint(&painter);
        painter.end();
        QCOMPARE(qAlpha(image.pixel(300, 100)), 128);

        view.setBackground(QColor(0x1e, 0x1e, 0x2e));
        QVERIFY(view.opaquePainting());
    }

    void testFontOverrides()
    {
        TerminalView view;
        const QFont desktop = view.font();
        view.setFontSize(23);
        QCOMPARE(view.font().pointSizeF(), 23.0);
        QCOMPARE(view.font().family(), desktop.family());
        view.setFontFamily(QStringLiteral("DejaVu Sans Mono"));
        QCOMPARE(view.font().family(), QStringLiteral("DejaVu Sans Mono"));
        view.setFontSize(0);
        QCOMPARE(view.font().pointSizeF(), desktop.pointSizeF());
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
