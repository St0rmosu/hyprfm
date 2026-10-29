#include <QTest>
#include <functional>
#include <csignal>
#include <QFile>
#include <QWheelEvent>
#include <QClipboard>
#include <QFontMetricsF>
#include <QGuiApplication>
#include <QMouseEvent>
#include <QDir>
#include <QFileInfo>
#include <QBuffer>
#include <QImage>
#include <QKeyEvent>
#include <QPainter>
#include <QQuickWindow>
#include <QSet>
#include <QSignalSpy>
#include <QStyleHints>
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

    static QImage render(TerminalView &view)
    {
        QImage image(view.size().toSize(), QImage::Format_RGB32);
        QPainter painter(&image);
        view.paint(&painter);
        return image;
    }

    static int countPixels(const QImage &image, const std::function<bool(QColor)> &match)
    {
        int n = 0;
        for (int y = 0; y < image.height(); ++y)
            for (int x = 0; x < image.width(); ++x)
                n += match(image.pixelColor(x, y));
        return n;
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

    // A skin-toned emoji is one grapheme in two cells (mode 2027, as in
    // kitty and Ghostty), so the letter after it sits in the third cell.
    void testEmojiClustersIntoOneCell()
    {
        TerminalView view;
        view.setSize(QSizeF(640, 240));
        view.start();
        view.sendText(QStringLiteral("clear; printf '\\360\\237\\221\\215\\360\\237\\217\\275X'; sleep 5\n"));
        QTest::qWait(400);
        render(view);
        mouse(view, QEvent::MouseButtonPress, cellCentre(view, 0, 0), 1000);
        const QPointF end = cellCentre(view, 2, 0) + QPointF(2, 0);
        mouse(view, QEvent::MouseMove, end, 1010);
        mouse(view, QEvent::MouseButtonRelease, end, 1020, Qt::NoButton);
        QCOMPARE(view.selectedText(), QString::fromUtf8("\U0001F44D\U0001F3FDX"));
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

    // A bar cursor covers the left edge of its cell only; a block, all of it.
    void testCursorStyle()
    {
        QQuickWindow window;
        window.resize(320, 120);
        TerminalView view(window.contentItem());
        view.setSize(QSizeF(320, 120));
        view.setBackground(Qt::black);
        view.setForeground(Qt::white);
        window.show();
        view.forceActiveFocus();
        QTRY_VERIFY(view.hasActiveFocus());
        const QFontMetricsF metrics(view.font());
        const int cell = qCeil(metrics.horizontalAdvance(QLatin1Char('M')));
        const auto litAt = [&](int x) {
            QImage image(320, 120, QImage::Format_RGB32);
            QPainter painter(&image);
            view.paint(&painter);
            painter.end();
            return qGray(image.pixel(x, 6 + qCeil(metrics.height()) / 2)) > 60;
        };
        // Cursor at the top-left cell of an empty screen.
        QVERIFY(litAt(6 + 1));
        QVERIFY(litAt(6 + cell - 2));
        view.setCursorStyle(QStringLiteral("bar"));
        QVERIFY(litAt(6 + 1));
        QVERIFY(!litAt(6 + cell - 2));

        // Blinking: the cursor comes and goes while focused.
        view.setCursorBlink(true);
        QVERIFY(litAt(6 + 1));
        QTRY_VERIFY(!litAt(6 + 1));
        QTRY_VERIFY(litAt(6 + 1));
        // Off again: steady, whatever phase it stopped in.
        view.setCursorBlink(false);
        QVERIFY(litAt(6 + 1));
        QTest::qWait(QGuiApplication::styleHints()->cursorFlashTime());
        QVERIFY(litAt(6 + 1));
    }

    // The theme's palette replaces the ANSI colours: red (colour 1) set to
    // pure green paints green.
    void testThemePaletteColoursAnsiText()
    {
        TerminalView view;
        view.setSize(QSizeF(640, 240));
        view.setBackground(Qt::black);
        QVariantList palette;
        for (int i = 0; i < 16; ++i)
            palette.append(i == 1 ? QColor(0, 255, 0) : QColor(Qt::gray));
        view.setAnsiPalette(palette);
        view.start();
        view.sendText(QStringLiteral("clear; printf '\\033[31mXXXXXXXXXXXXXXXXXXXX\\033[0m\\n'\n"));
        const auto green = [](QColor c) { return c.green() > 150 && c.red() < 100 && c.blue() < 100; };
        QTRY_VERIFY_WITH_TIMEOUT(countPixels(render(view), green) > 50, 5000);

        // A palette of the wrong size is ignored: back to Ghostty's red.
        view.setAnsiPalette({});
        QCOMPARE(countPixels(render(view), green), 0);
    }

    // SGR 4:1-4:5 draw five different underlines under the same text, and
    // SGR 58 recolours them. Each style is told apart by its pixel profile
    // under the text: rows lit (double has two bands) and columns lit
    // (dotted and dashed leave gaps, a wave moves between rows).
    void testUnderlineStyles()
    {
        TerminalView view;
        view.setSize(QSizeF(640, 240));
        view.setBackground(Qt::black);
        view.setForeground(Qt::white);
        view.start();
        // Row r (0-4) holds 20 spaces in underline style r+1, coloured pure
        // red with SGR 58 so only the line itself is red.
        view.sendText(QStringLiteral("clear; for s in 1 2 3 4 5; do printf '\\033[4:%sm\\033[58:2::255:0:0m%20s\\033[0m\\n' $s ''; done; sleep 5\n"));
        const QFontMetricsF metrics(view.font());
        const int cellH = qCeil(metrics.height());
        const int cellW = qCeil(metrics.horizontalAdvance(QLatin1Char('M')));
        QImage image;
        // Anything reddish: the wave is antialiased, and nothing else on
        // the black screen is red.
        const auto isRed = [](QColor c) { return c.red() > 50 && c.green() < 40 && c.blue() < 40; };
        QTRY_VERIFY_WITH_TIMEOUT((image = render(view), countPixels(image, isRed) > 5 * 15 * cellW / 2), 5000);
        if (qEnvironmentVariableIsSet("HYPRFM_TEST_GRAB"))
            image.save(qEnvironmentVariable("HYPRFM_TEST_GRAB"));
        struct Profile { int bands = 0; int litColumns = 0; int distinctRows = 0; };
        const auto profile = [&](int row) {
            Profile p;
            const int top = 6 + row * cellH;
            const int x0 = 6, x1 = 6 + 20 * cellW;
            bool inBand = false;
            QSet<int> rows;
            for (int y = top; y < top + cellH; ++y) {
                bool lit = false;
                for (int x = x0; x < x1; ++x)
                    lit |= isRed(image.pixelColor(x, y));
                if (lit && !inBand)
                    ++p.bands;
                inBand = lit;
            }
            for (int x = x0; x < x1; ++x) {
                for (int y = top; y < top + cellH; ++y) {
                    if (isRed(image.pixelColor(x, y))) {
                        ++p.litColumns;
                        rows.insert(y);
                        break;
                    }
                }
            }
            p.distinctRows = rows.size();
            return p;
        };
        const int width = 20 * cellW;
        const Profile single = profile(0), dbl = profile(1), curly = profile(2), dotted = profile(3),
                      dashed = profile(4);
        QCOMPARE(single.bands, 1);
        QVERIFY(single.litColumns > width - 2);
        QCOMPARE(dbl.bands, 2);
        QVERIFY2(curly.distinctRows >= 3, qPrintable(QString::number(curly.distinctRows)));
        QVERIFY(curly.litColumns > width - 2);
        QVERIFY(dotted.litColumns < width * 0.7 && dotted.litColumns > width * 0.3);
        QVERIFY(dashed.litColumns < width * 0.7 && dashed.litColumns > width * 0.3);
    }

    // BEL flashes the view once, however fast it rings.
    void testBellFlashes()
    {
        TerminalView view;
        view.setSize(QSizeF(320, 120));
        view.setBackground(Qt::black);
        view.setForeground(Qt::white);
        QSignalSpy bell(&view, &TerminalView::bell);
        view.start();
        view.sendText(QStringLiteral("clear; printf '\\a\\a\\a'; sleep 5\n"));
        QTRY_COMPARE_WITH_TIMEOUT(bell.count(), 1, 5000);
        render(view);
        QVERIFY(qGray(render(view).pixel(300, 100)) > 10);   // tinted
        QTRY_VERIFY(qGray(render(view).pixel(300, 100)) == 0);   // and back
        QCOMPARE(bell.count(), 1);
    }

    // A program that asked for focus reports (mode 1004) reads CSI I when
    // the view gains focus and CSI O when it loses it, once each.
    void testFocusIsReported()
    {
        QTemporaryDir dir;
        QQuickWindow window;
        window.resize(320, 120);
        TerminalView view(window.contentItem());
        view.setSize(QSizeF(320, 120));
        QQuickItem other(window.contentItem());
        other.setFlag(QQuickItem::ItemIsFocusScope);
        other.setActiveFocusOnTab(true);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        view.start();
        const QString out = dir.filePath("focus");
        view.sendText(QStringLiteral("printf '\\033[?1004h'; stty -icanon min 1; dd bs=1 count=6 of='%1' 2>/dev/null\n").arg(out));
        QTest::qWait(500);
        view.forceActiveFocus();
        QTRY_VERIFY(view.hasActiveFocus());
        view.forceActiveFocus();   // no second report
        other.forceActiveFocus();
        QTRY_VERIFY(!view.hasActiveFocus());
        QTRY_COMPARE_WITH_TIMEOUT(QFileInfo(out).size(), 6, 5000);
        QFile file(out);
        QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(file.readAll(), QByteArray("\x1b[I\x1b[O"));
    }

    // OSC 52 sets the clipboard; asking to read it ("?") gets nothing, so a
    // program cannot lift what the user copied.
    void testOsc52WritesButNeverReadsTheClipboard()
    {
        QTemporaryDir dir;
        TerminalView view;
        view.setSize(QSizeF(640, 240));
        QGuiApplication::clipboard()->setText(QStringLiteral("secret"));
        view.start();
        // "aGVsbG8=" is base64 for "hello".
        view.sendText(QStringLiteral("printf '\\033]52;c;aGVsbG8=\\a'\n"));
        QTRY_COMPARE_WITH_TIMEOUT(QGuiApplication::clipboard()->text(), QStringLiteral("hello"), 5000);

        // A read request: whatever comes back within a second goes to a
        // file, which must not contain the clipboard.
        QGuiApplication::clipboard()->setText(QStringLiteral("secret"));
        const QString out = dir.filePath("reply");
        view.sendText(QStringLiteral("stty -icanon -echo min 0 time 10; printf '\\033]52;c;?\\a'; dd bs=1 count=64 of='%1' 2>/dev/null; touch '%1.done'\n").arg(out));
        QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(out + ".done"), 5000);
        QFile file(out);
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QByteArray reply = file.readAll();
        QVERIFY2(!reply.contains("c2VjcmV0"), reply.constData());   // base64 "secret"
        QCOMPARE(QGuiApplication::clipboard()->text(), QStringLiteral("secret"));
    }

    // OSC 9 and OSC 777 reach the notification signal as plain text, and
    // a burst of them makes one notification, not a flood.
    void testNotifications()
    {
        TerminalView view;
        view.setSize(QSizeF(640, 240));
        view.setProperty("desktopNotifications", false);   // never the live desktop's server
        QSignalSpy notified(&view, &TerminalView::notification);
        view.start();
        view.sendText(QStringLiteral("printf '\\033]777;notify;Build;done <b>ok</b>\\a'; for i in 1 2 3 4 5; do printf '\\033]9;spam\\a'; done\n"));
        QTRY_COMPARE_WITH_TIMEOUT(notified.count(), 1, 5000);
        QCOMPARE(notified.at(0).at(0).toString(), QStringLiteral("Build"));
        QCOMPARE(notified.at(0).at(1).toString(), QStringLiteral("done <b>ok</b>"));
        QTest::qWait(300);
        QCOMPARE(notified.count(), 1);
        QTest::qWait(1000);
        view.sendText(QStringLiteral("printf '\\033]9;later\\a'\n"));
        QTRY_COMPARE_WITH_TIMEOUT(notified.count(), 2, 5000);
        QCOMPARE(notified.at(1).at(1).toString(), QStringLiteral("later"));
    }

    static void hover(TerminalView &view, QPointF pos)
    {
        QHoverEvent event(QEvent::HoverMove, pos, view.mapToGlobal(pos), QPointF(-1, -1));
        QCoreApplication::sendEvent(&view, &event);
    }

    static void ctrlClick(TerminalView &view, QPointF pos)
    {
        QMouseEvent press(QEvent::MouseButtonPress, pos, pos, view.mapToGlobal(pos), Qt::LeftButton,
                          Qt::LeftButton, Qt::ControlModifier);
        QCoreApplication::sendEvent(&view, &press);
        QMouseEvent release(QEvent::MouseButtonRelease, pos, pos, view.mapToGlobal(pos), Qt::LeftButton,
                            Qt::NoButton, Qt::ControlModifier);
        QCoreApplication::sendEvent(&view, &release);
    }

    // Prints `printfArgs` on the top row of a cleared screen and paints once.
    static void showRow(TerminalView &view, const QString &printfArgs)
    {
        view.setSize(QSizeF(900, 240));
        view.start();
        view.sendText(QStringLiteral("clear; printf %1; sleep 5\n").arg(printfArgs));
        QTest::qWait(500);
        render(view);
    }

    // An OSC 8 link shows its real target on hover and opens on Ctrl+click.
    void testOsc8LinkHoverAndCtrlClick()
    {
        TerminalView view;
        QSignalSpy opened(&view, &TerminalView::linkActivated);
        showRow(view, QStringLiteral("'\\033]8;;https://example.com/real\\033\\\\docs\\033]8;;\\033\\\\ after'"));
        hover(view, cellCentre(view, 1, 0));
        QCOMPARE(view.hoveredLink(), QStringLiteral("https://example.com/real"));
        hover(view, cellCentre(view, 6, 0));   // "after" is plain text
        QCOMPARE(view.hoveredLink(), QString());
        ctrlClick(view, cellCentre(view, 2, 0));
        QCOMPARE(opened.count(), 1);
        QCOMPARE(opened.at(0).at(0).toString(), QStringLiteral("https://example.com/real"));
    }

    // The label shows the target percent-encoded, so a right-to-left
    // override (U+202E) cannot make it read as another address.
    void testLinkLabelEncodesBidi()
    {
        TerminalView view;
        showRow(view, QStringLiteral("'\\033]8;;https://example.com/\\342\\200\\256moc.live\\033\\\\docs\\033]8;;\\033\\\\'"));
        hover(view, cellCentre(view, 1, 0));
        QCOMPARE(view.hoveredLink(), QStringLiteral("https://example.com/%E2%80%AEmoc.live"));
    }

    // HyprFM started from `hyprfm &` in a script ignores SIGINT; its shells
    // must not, or Ctrl+C never stops anything.
    void testShellGetsDefaultSignals()
    {
        QTemporaryDir dir;
        const QString out = dir.filePath(QStringLiteral("sigign"));
        struct sigaction ignore{}, old{};
        ignore.sa_handler = SIG_IGN;
        sigaction(SIGINT, &ignore, &old);
        TerminalView view;
        view.setSize(QSizeF(640, 240));
        view.start();
        sigaction(SIGINT, &old, nullptr);
        view.sendText(QStringLiteral("grep SigIgn /proc/self/status > %1\n").arg(out));
        QTRY_VERIFY_WITH_TIMEOUT(QFileInfo(out).size() > 0, 5000);
        QFile file(out);
        QVERIFY(file.open(QIODevice::ReadOnly));
        const quint64 ignored = file.readAll().split('\t').last().trimmed().toULongLong(nullptr, 16);
        QCOMPARE(ignored & (1u << (SIGINT - 1)), 0u);
    }

    // The shell does not inherit the identity of the terminal HyprFM was
    // started from.
    void testShellDropsOuterTerminalEnvironment()
    {
        QTemporaryDir dir;
        const QString out = dir.filePath(QStringLiteral("env"));
        qputenv("TMUX", "/tmp/tmux-1000/default,1,0");
        qputenv("TERM_PROGRAM", "WezTerm");
        TerminalView view;
        view.setSize(QSizeF(640, 240));
        view.start();
        qunsetenv("TMUX");
        qunsetenv("TERM_PROGRAM");
        view.sendText(QStringLiteral("echo \"[$TMUX][$TERM_PROGRAM][$TERM]\" > %1\n").arg(out));
        QTRY_VERIFY_WITH_TIMEOUT(QFileInfo(out).size() > 0, 5000);
        QFile file(out);
        QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(file.readAll().trimmed(), QByteArray("[][][xterm-256color]"));
    }

    // A URL written out in the text is a link too, without the sentence
    // punctuation or a closing bracket around it.
    void testPlainUrlIsALink()
    {
        TerminalView view;
        QSignalSpy opened(&view, &TerminalView::linkActivated);
        showRow(view, QStringLiteral("'(see https://example.com/a_(b)/c?x=1).'"));
        hover(view, cellCentre(view, 10, 0));
        QCOMPARE(view.hoveredLink(), QStringLiteral("https://example.com/a_(b)/c?x=1"));
        ctrlClick(view, cellCentre(view, 10, 0));
        QCOMPARE(opened.count(), 1);
        // A plain click selects, as before; it opens nothing.
        mouse(view, QEvent::MouseButtonPress, cellCentre(view, 10, 0), 1000);
        mouse(view, QEvent::MouseButtonRelease, cellCentre(view, 10, 0), 1010, Qt::NoButton);
        QCOMPARE(opened.count(), 1);
    }

    // file:// links go to HyprFM as a folder: the file's own folder, never
    // the file itself, and only for this machine.
    void testFileLinkOpensItsFolder()
    {
        QTemporaryDir dir;
        QFile file(dir.filePath("notes.txt"));
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.close();
        TerminalView view;
        QSignalSpy folder(&view, &TerminalView::folderActivated);
        QSignalSpy opened(&view, &TerminalView::linkActivated);
        showRow(view, QStringLiteral("'\\033]8;;file://localhost%1\\033\\\\local\\033]8;;\\033\\\\ \\033]8;;file://elsewhere.example%1\\033\\\\remote\\033]8;;\\033\\\\'")
                              .arg(file.fileName()));
        ctrlClick(view, cellCentre(view, 1, 0));
        QCOMPARE(folder.count(), 1);
        QCOMPARE(folder.at(0).at(0).toString(), dir.path());
        ctrlClick(view, cellCentre(view, 8, 0));
        QCOMPARE(folder.count(), 1);
        QCOMPARE(opened.count(), 0);
    }

    // Links to anything but the web, mail or this machine's files do nothing.
    void testUnsafeLinksAreRefused()
    {
        TerminalView view;
        QSignalSpy opened(&view, &TerminalView::linkActivated);
        QSignalSpy folder(&view, &TerminalView::folderActivated);
        showRow(view, QStringLiteral("'\\033]8;;javascript:alert(1)\\033\\\\aaaa\\033]8;;\\033\\\\ \\033]8;;ssh://host/x\\033\\\\bbbb\\033]8;;\\033\\\\ \\033]8;;smb://host/share\\033\\\\cccc\\033]8;;\\033\\\\'"));
        for (int column : {1, 6, 11})
            ctrlClick(view, cellCentre(view, column, 0));
        QCOMPARE(opened.count(), 0);
        QCOMPARE(folder.count(), 0);
    }

    // A mailto link loses every field but subject, body and cc: some mail
    // clients would attach a local file named by attach=.
    void testMailtoLinkDropsAttachments()
    {
        TerminalView view;
        QSignalSpy opened(&view, &TerminalView::linkActivated);
        showRow(view, QStringLiteral("'\\033]8;;mailto:me@example.com?subject=Hi&attach=/etc/passwd&Attachment=%%2Fx&body=b\\033\\\\mail\\033]8;;\\033\\\\'"));
        ctrlClick(view, cellCentre(view, 1, 0));
        QCOMPARE(opened.count(), 1);
        QCOMPARE(opened.at(0).at(0).toString(), QStringLiteral("mailto:me@example.com?subject=Hi&body=b"));
    }

    // Find covers the scrollback, ignores ASCII case, steps from the newest
    // match up into older output and wraps both ways, and tints matches.
    void testSearchScrollback()
    {
        TerminalView view;
        view.setSize(QSizeF(640, 240));
        view.setBackground(Qt::black);
        view.setProperty("matchColor", QColor(0, 255, 0));
        view.start();
        // Rows 5, 20 and 38 of 40 say "needle"; the typed command must not,
        // so the word is put together by the shell.
        view.sendText(QStringLiteral("clear; n=nee; for i in $(seq 1 40); do case $i in 5|20|38) echo \"row $i ${n}dle\";; *) echo \"row $i\";; esac; done; sleep 5\n"));
        QTest::qWait(800);
        QSignalSpy changed(&view, &TerminalView::searchChanged);
        view.setSearchText(QStringLiteral("NEEDLE"));
        QCOMPARE(view.searchMatchCount(), 3);
        QCOMPARE(view.searchMatchIndex(), 1);   // the newest, row 38
        QVERIFY(changed.count() > 0);
        const auto green = [](QColor c) { return c.green() > 100 && c.red() < 60 && c.blue() < 60; };
        QVERIFY(countPixels(render(view), green) > 20);

        view.findNext();   // row 20, in the scrollback: the view scrolls to it
        QCOMPARE(view.searchMatchIndex(), 2);
        QVERIFY(countPixels(render(view), green) > 20);
        view.findNext();
        QCOMPARE(view.searchMatchIndex(), 3);
        view.findNext();   // wraps to the newest
        QCOMPARE(view.searchMatchIndex(), 1);
        view.findPrevious();   // and back the other way
        QCOMPARE(view.searchMatchIndex(), 3);

        view.setSearchText(QStringLiteral("no such text"));
        QCOMPARE(view.searchMatchCount(), 0);
        QCOMPARE(view.searchMatchIndex(), 0);
        view.setSearchText(QString());
        QCOMPARE(view.searchMatchCount(), 0);
        QCOMPARE(countPixels(render(view), green), 0);
    }

    // The scroll state follows output and scrolling, and scrollToRow goes
    // to a row of the scrollback.
    void testScrollbarState()
    {
        TerminalView view;
        view.setSize(QSizeF(640, 240));
        QSignalSpy changed(&view, &TerminalView::scrollChanged);
        view.start();
        view.sendText(QStringLiteral("clear; seq 1 100; sleep 5\n"));
        QTRY_VERIFY_WITH_TIMEOUT(view.scrollTotal() > 100, 5000);
        QVERIFY(changed.count() > 0);
        QCOMPARE(view.scrollOffset() + view.scrollLength(), view.scrollTotal());   // at the bottom
        view.scrollToRow(0);
        QCOMPARE(view.scrollOffset(), 0.0);
        view.scrollToRow(10);
        QCOMPARE(view.scrollOffset(), 10.0);
        // Typing returns to the prompt.
        type(view, QStringLiteral(" "));
        QCOMPARE(view.scrollOffset() + view.scrollLength(), view.scrollTotal());
    }

    // Shift+PageUp/PageDown page the scrollback, Shift+Home/End go to its
    // ends, Ctrl+Shift+PageUp/PageDown jump between OSC 133 prompts.
    void testScrollKeys()
    {
        TerminalView view;
        view.setSize(QSizeF(640, 240));
        view.start();
        view.sendText(QStringLiteral("clear; for p in 1 2; do printf '\\033]133;A\\007P%s\\n' $p; seq 1 60; done; sleep 5\n"));
        QTRY_VERIFY_WITH_TIMEOUT(view.scrollTotal() > 120, 5000);
        auto press = [&](int key, Qt::KeyboardModifiers mods) {
            QKeyEvent event(QEvent::KeyPress, key, mods);
            QCoreApplication::sendEvent(&view, &event);
        };
        const qreal bottom = view.scrollOffset();
        press(Qt::Key_PageUp, Qt::ShiftModifier);
        QCOMPARE(view.scrollOffset(), bottom - (view.scrollLength() - 1));
        press(Qt::Key_PageDown, Qt::ShiftModifier);
        QCOMPARE(view.scrollOffset(), bottom);
        press(Qt::Key_Home, Qt::ShiftModifier);
        QCOMPARE(view.scrollOffset(), 0.0);
        press(Qt::Key_End, Qt::ShiftModifier);
        QCOMPARE(view.scrollOffset(), bottom);

        press(Qt::Key_PageUp, Qt::ControlModifier | Qt::ShiftModifier);
        const qreal second = view.scrollOffset();
        QVERIFY(second < bottom);
        press(Qt::Key_PageUp, Qt::ControlModifier | Qt::ShiftModifier);
        const qreal first = view.scrollOffset();
        QVERIFY(first < second);
        QCOMPARE(second - first, 61.0);   // P1 and its 60 lines
        press(Qt::Key_PageDown, Qt::ControlModifier | Qt::ShiftModifier);
        QCOMPARE(view.scrollOffset(), second);
    }

    // Kitty graphics: raw pixels and PNGs sent inline are drawn; a file
    // the program names is never opened.
    void testKittyGraphics()
    {
        QTemporaryDir dir;
        QImage blue(20, 20, QImage::Format_RGB32);
        blue.fill(Qt::blue);
        const QString path = dir.filePath(QStringLiteral("blue.png"));
        QVERIFY(blue.save(path));
        QByteArray png;
        QBuffer buffer(&png);
        buffer.open(QIODevice::WriteOnly);
        QImage green(20, 20, QImage::Format_RGB32);
        green.fill(Qt::green);
        QVERIFY(green.save(&buffer, "PNG"));
        QByteArray red;
        for (int i = 0; i < 20 * 20; ++i)
            red.append("\xff\x00\x00", 3);

        TerminalView view;
        view.setSize(QSizeF(640, 240));
        view.setBackground(Qt::black);
        view.setForeground(Qt::white);
        view.start();
        const QString apc = QStringLiteral("printf '\\033_G%1;%2\\033\\\\'; ");
        view.sendText(QStringLiteral("clear; ")
                      + apc.arg(QStringLiteral("a=T,f=24,s=20,v=20,q=2"), QString::fromLatin1(red.toBase64()))
                      + apc.arg(QStringLiteral("a=T,f=100,q=2"), QString::fromLatin1(png.toBase64()))
                      + apc.arg(QStringLiteral("a=T,f=100,t=f,q=2"), QString::fromLatin1(path.toUtf8().toBase64()))
                      + QStringLiteral("echo; sleep 5\n"));
        const auto isRed = [](QColor c) { return c.red() > 200 && c.green() < 50 && c.blue() < 50; };
        const auto isGreen = [](QColor c) { return c.green() > 200 && c.red() < 50 && c.blue() < 50; };
        const auto isBlue = [](QColor c) { return c.blue() > 200 && c.red() < 50 && c.green() < 50; };
        QImage image;
        QTRY_VERIFY_WITH_TIMEOUT((image = render(view), countPixels(image, isGreen) >= 400), 5000);
        QCOMPARE(countPixels(image, isRed), 400);
        QCOMPARE(countPixels(image, isGreen), 400);
        QCOMPARE(countPixels(image, isBlue), 0);
    }

    // A new tab opens where the shell is, which bash only tells /proc.
    void testCurrentDirectoryAndTabKeys()
    {
        TerminalView view;
        view.setSize(QSizeF(640, 240));
        view.setWorkingDirectory(QDir::homePath());
        view.start();
        QTRY_COMPARE_WITH_TIMEOUT(view.currentDirectory(), QDir::homePath(), 5000);
        view.sendText(QStringLiteral("cd /usr\n"));
        QTRY_COMPARE_WITH_TIMEOUT(view.currentDirectory(), QStringLiteral("/usr"), 5000);

        QSignalSpy newTab(&view, &TerminalView::newTabRequested);
        QSignalSpy closeTab(&view, &TerminalView::closeTabRequested);
        QSignalSpy switchTab(&view, &TerminalView::switchTabRequested);
        auto press = [&](int key, Qt::KeyboardModifiers mods) {
            QKeyEvent event(QEvent::KeyPress, key, mods);
            QCoreApplication::sendEvent(&view, &event);
        };
        press(Qt::Key_T, Qt::ControlModifier | Qt::ShiftModifier);
        press(Qt::Key_W, Qt::ControlModifier | Qt::ShiftModifier);
        press(Qt::Key_PageUp, Qt::ControlModifier);   // one tab: the program's
        QCOMPARE(switchTab.count(), 0);
        view.setProperty("tabSwitching", true);
        press(Qt::Key_PageUp, Qt::ControlModifier);
        QCOMPARE(newTab.count(), 1);
        QCOMPARE(closeTab.count(), 1);
        QCOMPARE(switchTab.count(), 1);
        QCOMPARE(switchTab.at(0).at(0).toInt(), -1);
    }

    // History past scrollbackLines is dropped (by whole pages, so loosely).
    void testScrollbackLimit()
    {
        TerminalView view;
        view.setSize(QSizeF(640, 240));
        view.setScrollbackLines(1000);
        view.start();
        view.sendText(QStringLiteral("seq 1 20000; echo done; sleep 5\n"));
        QTRY_VERIFY_WITH_TIMEOUT(view.scrollTotal() > 500, 5000);
        QTest::qWait(1500);
        QVERIFY2(view.scrollTotal() < 5000, qPrintable(QString::number(view.scrollTotal())));
    }

    // Ctrl+= and Ctrl+- zoom by a point within 4-72, Ctrl+0 resets.
    void testZoomKeys()
    {
        TerminalView view;
        view.setSize(QSizeF(640, 240));
        view.setFontSize(10);
        view.start();
        auto press = [&](int key) {
            QKeyEvent event(QEvent::KeyPress, key, Qt::ControlModifier);
            QCoreApplication::sendEvent(&view, &event);
        };
        press(Qt::Key_Equal);
        press(Qt::Key_Equal);
        QCOMPARE(view.font().pointSizeF(), 12.0);
        press(Qt::Key_Minus);
        QCOMPARE(view.font().pointSizeF(), 11.0);
        for (int i = 0; i < 20; ++i)
            press(Qt::Key_Minus);
        QCOMPARE(view.font().pointSizeF(), 4.0);
        press(Qt::Key_Equal);   // back up at once from the limit
        QCOMPARE(view.font().pointSizeF(), 5.0);
        press(Qt::Key_0);
        QCOMPARE(view.font().pointSizeF(), 10.0);
        QCOMPARE(view.fontSize(), 10.0);   // the setting itself is untouched
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
