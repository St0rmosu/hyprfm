#pragma once

#include <QColor>
#include <QFont>
#include <QQuickPaintedItem>
#include <QStringList>
#include <QtQml/qqmlregistration.h>

// Ghostty names a struct field `emit`, which Qt defines away as a macro.
#pragma push_macro("emit")
#undef emit
#include <ghostty/vt.h>
#pragma pop_macro("emit")

class QSocketNotifier;

// A shell in a pty, emulated by libghostty-vt (Ghostty's terminal core) and
// painted as a QML item. Ghostty does the parsing, terminal state, scrollback
// and key encoding; this class is the pty, the painting and the Qt input.
class TerminalView : public QQuickPaintedItem
{
    Q_OBJECT
    QML_ELEMENT

    // Where the shell starts. Later changes are not sent to a running shell;
    // see changeDirectory().
    Q_PROPERTY(QString workingDirectory READ workingDirectory WRITE setWorkingDirectory
                   NOTIFY workingDirectoryChanged)
    // Where the shell says it is (OSC 7), empty if it never says.
    Q_PROPERTY(QString shellDirectory READ shellDirectory NOTIFY shellDirectoryChanged)
    Q_PROPERTY(QString title READ title NOTIFY titleChanged)
    Q_PROPERTY(bool running READ running NOTIFY runningChanged)
    Q_PROPERTY(QFont font READ font WRITE setFont NOTIFY fontChanged)
    Q_PROPERTY(QColor foreground READ foreground WRITE setForeground NOTIFY colorsChanged)
    Q_PROPERTY(QColor background READ background WRITE setBackground NOTIFY colorsChanged)
    // Shortcuts the window keeps while the terminal has focus (the panel
    // toggle, say); every other key goes to the shell, Ctrl+C included.
    Q_PROPERTY(QStringList windowShortcuts MEMBER m_windowShortcuts)

public:
    explicit TerminalView(QQuickItem *parent = nullptr);
    ~TerminalView() override;

    QString workingDirectory() const { return m_workingDirectory; }
    void setWorkingDirectory(const QString &dir);
    QString shellDirectory() const { return m_shellDirectory; }
    QString title() const { return m_title; }
    bool running() const { return m_ptyFd >= 0; }
    QFont font() const { return m_font; }
    void setFont(const QFont &font);
    QColor foreground() const { return m_foreground; }
    void setForeground(const QColor &color);
    QColor background() const { return m_background; }
    void setBackground(const QColor &color);

    // Starts the shell if it is not running.
    Q_INVOKABLE void start();
    // Types `text` into the shell as if pasted.
    Q_INVOKABLE void sendText(const QString &text);
    // Types a cd to `dir` if the shell is at its prompt (no program in the
    // foreground) and returns whether it did.
    Q_INVOKABLE bool changeDirectory(const QString &dir);

    void paint(QPainter *painter) override;

signals:
    void workingDirectoryChanged();
    void shellDirectoryChanged();
    void titleChanged();
    void runningChanged();
    void fontChanged();
    void colorsChanged();
    void finished();

protected:
    bool event(QEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void keyReleaseEvent(QKeyEvent *event) override;
    void inputMethodEvent(QInputMethodEvent *event) override;
    QVariant inputMethodQuery(Qt::InputMethodQuery query) const override;
    void mousePressEvent(QMouseEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void geometryChange(const QRectF &newGeometry, const QRectF &oldGeometry) override;

private:
    void readPty();
    void writePty(const char *data, qsizetype len);
    void stop();
    void sendKey(QKeyEvent *event, GhosttyKeyAction action);
    void updateCellSize();
    void resizeGrid();
    void applyColors();

    static void onWritePty(GhosttyTerminal, void *self, const uint8_t *data, size_t len);
    static bool onSize(GhosttyTerminal, void *self, GhosttySizeReportSize *out);
    static void onTitleChanged(GhosttyTerminal, void *self);
    static void onPwdChanged(GhosttyTerminal, void *self);

    GhosttyTerminal m_terminal = nullptr;
    GhosttyRenderState m_renderState = nullptr;
    GhosttyRenderStateRowIterator m_rows = nullptr;
    GhosttyRenderStateRowCells m_cells = nullptr;
    GhosttyKeyEncoder m_keyEncoder = nullptr;
    GhosttyKeyEvent m_keyEvent = nullptr;

    int m_ptyFd = -1;
    qint64 m_pid = -1;
    QSocketNotifier *m_notifier = nullptr;

    QString m_workingDirectory;
    QString m_shellDirectory;
    QString m_title;
    QStringList m_windowShortcuts;
    QFont m_font;
    QColor m_foreground = QColor(0xcd, 0xd6, 0xf4);
    QColor m_background = QColor(0x1e, 0x1e, 0x2e);
    qreal m_cellWidth = 8;
    qreal m_cellHeight = 16;
    qreal m_ascent = 12;
    uint16_t m_cols = 80;
    uint16_t m_rowCount = 24;
    qreal m_scrollRemainder = 0;
};
