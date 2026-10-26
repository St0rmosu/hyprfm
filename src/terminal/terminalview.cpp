#include "terminal/terminalview.h"

#include <QClipboard>
#include <QCursor>
#include <QFile>
#include <QFontDatabase>
#include <QFontMetricsF>
#include <QFontInfo>
#include <QGuiApplication>
#include <QInputMethodEvent>
#include <QKeyEvent>
#include <QKeySequence>
#include <QPainter>
#include <QSocketNotifier>
#include <QStyleHints>
#include <QUrl>
#include <QVarLengthArray>

// Ghostty structs have an `emit` field; this file signals with Q_EMIT.
#undef emit

#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <pty.h>
#include <pwd.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

constexpr int Padding = 6;

GhosttyKey ghosttyKey(int key)
{
    if (key >= Qt::Key_A && key <= Qt::Key_Z)
        return GhosttyKey(GHOSTTY_KEY_A + (key - Qt::Key_A));
    if (key >= Qt::Key_0 && key <= Qt::Key_9)
        return GhosttyKey(GHOSTTY_KEY_DIGIT_0 + (key - Qt::Key_0));
    if (key >= Qt::Key_F1 && key <= Qt::Key_F12)
        return GhosttyKey(GHOSTTY_KEY_F1 + (key - Qt::Key_F1));
    switch (key) {
    case Qt::Key_Space: return GHOSTTY_KEY_SPACE;
    case Qt::Key_Return:
    case Qt::Key_Enter: return GHOSTTY_KEY_ENTER;
    case Qt::Key_Tab:
    case Qt::Key_Backtab: return GHOSTTY_KEY_TAB;
    case Qt::Key_Backspace: return GHOSTTY_KEY_BACKSPACE;
    case Qt::Key_Delete: return GHOSTTY_KEY_DELETE;
    case Qt::Key_Escape: return GHOSTTY_KEY_ESCAPE;
    case Qt::Key_Up: return GHOSTTY_KEY_ARROW_UP;
    case Qt::Key_Down: return GHOSTTY_KEY_ARROW_DOWN;
    case Qt::Key_Left: return GHOSTTY_KEY_ARROW_LEFT;
    case Qt::Key_Right: return GHOSTTY_KEY_ARROW_RIGHT;
    case Qt::Key_Home: return GHOSTTY_KEY_HOME;
    case Qt::Key_End: return GHOSTTY_KEY_END;
    case Qt::Key_PageUp: return GHOSTTY_KEY_PAGE_UP;
    case Qt::Key_PageDown: return GHOSTTY_KEY_PAGE_DOWN;
    case Qt::Key_Insert: return GHOSTTY_KEY_INSERT;
    case Qt::Key_Minus: return GHOSTTY_KEY_MINUS;
    case Qt::Key_Equal: return GHOSTTY_KEY_EQUAL;
    case Qt::Key_BracketLeft: return GHOSTTY_KEY_BRACKET_LEFT;
    case Qt::Key_BracketRight: return GHOSTTY_KEY_BRACKET_RIGHT;
    case Qt::Key_Backslash: return GHOSTTY_KEY_BACKSLASH;
    case Qt::Key_Semicolon: return GHOSTTY_KEY_SEMICOLON;
    case Qt::Key_Apostrophe: return GHOSTTY_KEY_QUOTE;
    case Qt::Key_Comma: return GHOSTTY_KEY_COMMA;
    case Qt::Key_Period: return GHOSTTY_KEY_PERIOD;
    case Qt::Key_Slash: return GHOSTTY_KEY_SLASH;
    case Qt::Key_QuoteLeft: return GHOSTTY_KEY_BACKQUOTE;
    default: return GHOSTTY_KEY_UNIDENTIFIED;
    }
}

GhosttyMods ghosttyMods(Qt::KeyboardModifiers mods)
{
    GhosttyMods out = 0;
    if (mods & Qt::ShiftModifier)
        out |= GHOSTTY_MODS_SHIFT;
    if (mods & Qt::ControlModifier)
        out |= GHOSTTY_MODS_CTRL;
    if (mods & Qt::AltModifier)
        out |= GHOSTTY_MODS_ALT;
    if (mods & Qt::MetaModifier)
        out |= GHOSTTY_MODS_SUPER;
    return out;
}

GhosttyColorRgb rgb(const QColor &c)
{
    return GhosttyColorRgb{uint8_t(c.red()), uint8_t(c.green()), uint8_t(c.blue())};
}

QColor qcolor(const GhosttyColorRgb &c)
{
    return QColor(c.r, c.g, c.b);
}

// The desktop's fixed font, or fontconfig's monospace when the platform
// theme answers with a proportional one (HyprFM's does: Noto Sans), which
// would size every cell by a wide M.
QFont defaultTerminalFont()
{
    QFont font = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    if (!QFontInfo(font).fixedPitch()) {
        const qreal size = font.pointSizeF();
        font = QFont(QStringLiteral("monospace"));
        font.setStyleHint(QFont::Monospace);
        font.setFixedPitch(true);
        if (size > 0)
            font.setPointSizeF(size);
    }
    return font;
}

QString ghosttyString(GhosttyTerminal terminal, GhosttyTerminalData data)
{
    GhosttyString s{};
    if (ghostty_terminal_get(terminal, data, &s) != GHOSTTY_SUCCESS || !s.ptr)
        return {};
    return QString::fromUtf8(reinterpret_cast<const char *>(s.ptr), qsizetype(s.len));
}

} // namespace

TerminalView::TerminalView(QQuickItem *parent)
    : QQuickPaintedItem(parent)
    , m_font(defaultTerminalFont())
{
    setFlag(ItemAcceptsInputMethod);
    setFlag(ItemIsFocusScope);
    setAcceptedMouseButtons(Qt::LeftButton | Qt::MiddleButton);
    setActiveFocusOnTab(true);
    setOpaquePainting(true);
    updateCellSize();

    ghostty_terminal_new(nullptr, &m_terminal, m_cols, m_rowCount);
    ghostty_render_state_new(nullptr, &m_renderState);
    ghostty_render_state_row_iterator_new(nullptr, &m_rows);
    ghostty_render_state_row_cells_new(nullptr, &m_cells);
    ghostty_key_encoder_new(nullptr, &m_keyEncoder);
    ghostty_key_event_new(nullptr, &m_keyEvent);
    ghostty_selection_gesture_new(nullptr, &m_gesture);
    ghostty_selection_gesture_event_new(nullptr, &m_pressEvent, GHOSTTY_SELECTION_GESTURE_EVENT_TYPE_PRESS);
    ghostty_selection_gesture_event_new(nullptr, &m_dragEvent, GHOSTTY_SELECTION_GESTURE_EVENT_TYPE_DRAG);
    ghostty_selection_gesture_event_new(nullptr, &m_releaseEvent, GHOSTTY_SELECTION_GESTURE_EVENT_TYPE_RELEASE);
    setCursor(Qt::IBeamCursor);

    const size_t scrollback = 10000;
    ghostty_terminal_set(m_terminal, GHOSTTY_TERMINAL_OPT_SCROLLBACK_MAX_LINES, &scrollback);
    ghostty_terminal_set(m_terminal, GHOSTTY_TERMINAL_OPT_USERDATA, this);
    ghostty_terminal_set(m_terminal, GHOSTTY_TERMINAL_OPT_WRITE_PTY,
                         reinterpret_cast<const void *>(&TerminalView::onWritePty));
    ghostty_terminal_set(m_terminal, GHOSTTY_TERMINAL_OPT_SIZE,
                         reinterpret_cast<const void *>(&TerminalView::onSize));
    ghostty_terminal_set(m_terminal, GHOSTTY_TERMINAL_OPT_TITLE_CHANGED,
                         reinterpret_cast<const void *>(&TerminalView::onTitleChanged));
    ghostty_terminal_set(m_terminal, GHOSTTY_TERMINAL_OPT_PWD_CHANGED,
                         reinterpret_cast<const void *>(&TerminalView::onPwdChanged));
    applyColors();
}

TerminalView::~TerminalView()
{
    stop();
    ghostty_selection_gesture_event_free(m_releaseEvent);
    ghostty_selection_gesture_event_free(m_dragEvent);
    ghostty_selection_gesture_event_free(m_pressEvent);
    ghostty_selection_gesture_free(m_gesture, m_terminal);
    ghostty_key_event_free(m_keyEvent);
    ghostty_key_encoder_free(m_keyEncoder);
    ghostty_render_state_row_cells_free(m_cells);
    ghostty_render_state_row_iterator_free(m_rows);
    ghostty_render_state_free(m_renderState);
    ghostty_terminal_free(m_terminal);
}

void TerminalView::setWorkingDirectory(const QString &dir)
{
    if (m_workingDirectory == dir)
        return;
    m_workingDirectory = dir;
    Q_EMIT workingDirectoryChanged();
}

void TerminalView::setFont(const QFont &font)
{
    if (m_font == font)
        return;
    m_font = font;
    updateCellSize();
    resizeGrid();
    Q_EMIT fontChanged();
    update();
}

void TerminalView::setForeground(const QColor &color)
{
    if (m_foreground == color)
        return;
    m_foreground = color;
    applyColors();
    Q_EMIT colorsChanged();
}

void TerminalView::setBackground(const QColor &color)
{
    if (m_background == color)
        return;
    m_background = color;
    applyColors();
    Q_EMIT colorsChanged();
}

void TerminalView::applyColors()
{
    const GhosttyColorRgb fg = rgb(m_foreground);
    const GhosttyColorRgb bg = rgb(m_background);
    ghostty_terminal_set(m_terminal, GHOSTTY_TERMINAL_OPT_COLOR_FOREGROUND, &fg);
    ghostty_terminal_set(m_terminal, GHOSTTY_TERMINAL_OPT_COLOR_BACKGROUND, &bg);
    setFillColor(m_background);
    update();
}

void TerminalView::updateCellSize()
{
    // Cells are whole pixels, or columns drift apart across a wide line.
    const QFontMetricsF metrics(m_font);
    m_cellWidth = qCeil(metrics.horizontalAdvance(QLatin1Char('M')));
    m_cellHeight = qCeil(metrics.height());
    m_ascent = metrics.ascent();
}

void TerminalView::resizeGrid()
{
    const auto cols = uint16_t(qMax(2, int((width() - 2 * Padding) / m_cellWidth)));
    const auto rows = uint16_t(qMax(1, int((height() - 2 * Padding) / m_cellHeight)));
    if (cols == m_cols && rows == m_rowCount)
        return;
    m_cols = cols;
    m_rowCount = rows;
    ghostty_terminal_resize(m_terminal, cols, rows, uint32_t(m_cellWidth), uint32_t(m_cellHeight));
    if (m_ptyFd >= 0) {
        winsize ws{rows, cols, uint16_t(cols * m_cellWidth), uint16_t(rows * m_cellHeight)};
        ioctl(m_ptyFd, TIOCSWINSZ, &ws);
    }
    update();
}

void TerminalView::geometryChange(const QRectF &newGeometry, const QRectF &oldGeometry)
{
    QQuickPaintedItem::geometryChange(newGeometry, oldGeometry);
    if (newGeometry.size() != oldGeometry.size())
        resizeGrid();
}

void TerminalView::start()
{
    if (m_ptyFd >= 0)
        return;
    resizeGrid();

    QByteArray shell = qgetenv("SHELL");
    if (shell.isEmpty()) {
        const passwd *pw = getpwuid(getuid());
        shell = pw && pw->pw_shell && *pw->pw_shell ? QByteArray(pw->pw_shell) : QByteArray("/bin/sh");
    }
    const QByteArray dir = QFile::encodeName(m_workingDirectory);
    winsize ws{m_rowCount, m_cols, uint16_t(m_cols * m_cellWidth), uint16_t(m_rowCount * m_cellHeight)};

    int fd = -1;
    const pid_t pid = forkpty(&fd, nullptr, nullptr, &ws);
    if (pid < 0)
        return;
    if (pid == 0) {
        // Child: only async-signal-safe calls from here to exec. Nothing of
        // HyprFM's (its IPC socket, D-Bus, inotify) may leak into the shell,
        // and the shell must start with default signal handling.
        closefrom(3);
        sigset_t none;
        sigemptyset(&none);
        sigprocmask(SIG_SETMASK, &none, nullptr);
        signal(SIGPIPE, SIG_DFL);
        signal(SIGCHLD, SIG_DFL);
        if (!dir.isEmpty() && chdir(dir.constData()) != 0)
            (void)chdir("/");
        setenv("TERM", "xterm-256color", 1);
        setenv("COLORTERM", "truecolor", 1);
        unsetenv("COLUMNS");
        unsetenv("LINES");
        const char *name = strrchr(shell.constData(), '/');
        execl(shell.constData(), name ? name + 1 : shell.constData(), static_cast<char *>(nullptr));
        _exit(127);
    }

    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    m_ptyFd = fd;
    m_pid = pid;
    m_notifier = new QSocketNotifier(fd, QSocketNotifier::Read, this);
    connect(m_notifier, &QSocketNotifier::activated, this, &TerminalView::readPty);
    Q_EMIT runningChanged();
}

void TerminalView::stop()
{
    if (m_ptyFd < 0)
        return;
    delete m_notifier;
    m_notifier = nullptr;
    close(m_ptyFd);
    m_ptyFd = -1;
    if (m_pid > 0) {
        // Closing the pty already hangs the shell up; give it a moment to
        // save its history, then make sure it is gone and reaped.
        kill(pid_t(m_pid), SIGHUP);
        for (int i = 0; i < 20 && waitpid(pid_t(m_pid), nullptr, WNOHANG) == 0; ++i)
            usleep(10000);
        if (waitpid(pid_t(m_pid), nullptr, WNOHANG) == 0) {
            kill(pid_t(m_pid), SIGKILL);
            waitpid(pid_t(m_pid), nullptr, 0);
        }
        m_pid = -1;
    }
    Q_EMIT runningChanged();
}

void TerminalView::readPty()
{
    char buf[16384];
    for (;;) {
        const ssize_t n = read(m_ptyFd, buf, sizeof buf);
        if (n > 0) {
            ghostty_terminal_vt_write(m_terminal, reinterpret_cast<const uint8_t *>(buf), size_t(n));
            continue;
        }
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0 && errno == EAGAIN)
            break;
        // EOF, or EIO once the shell has exited.
        stop();
        Q_EMIT finished();
        break;
    }
    update();
}

void TerminalView::writePty(const char *data, qsizetype len)
{
    while (m_ptyFd >= 0 && len > 0) {
        const ssize_t n = write(m_ptyFd, data, size_t(len));
        if (n > 0) {
            data += n;
            len -= n;
        } else if (n < 0 && errno != EINTR) {
            break;   // ponytail: a full pty buffer drops the rest, as Ghostling does
        }
    }
}

void TerminalView::sendText(const QString &text)
{
    const QByteArray utf8 = text.toUtf8();
    writePty(utf8.constData(), utf8.size());
}

bool TerminalView::changeDirectory(const QString &dir)
{
    // The shell owns the terminal's foreground process group only while
    // nothing it started is running, so an editor never gets a cd typed in.
    if (m_ptyFd < 0 || tcgetpgrp(m_ptyFd) != pid_t(m_pid))
        return false;
    // The path is typed into the line editor, where control characters are
    // keys: a folder named "x^C^Mrm -rf ~" would cancel the cd and run the
    // rest. Quoting cannot stop that, so such paths are not typed at all.
    for (const QChar c : dir) {
        if (c.category() == QChar::Other_Control)
            return false;
    }
    QString quoted = dir;
    quoted.replace(QLatin1Char('\''), QLatin1String("'\\''"));
    // Ctrl+U drops whatever is half-typed; the leading space keeps the cd
    // out of history in shells that ignore space-prefixed lines.
    sendText(QStringLiteral("\x15 cd -- '%1'\r").arg(quoted));
    return true;
}

void TerminalView::onWritePty(GhosttyTerminal, void *self, const uint8_t *data, size_t len)
{
    static_cast<TerminalView *>(self)->writePty(reinterpret_cast<const char *>(data), qsizetype(len));
}

bool TerminalView::onSize(GhosttyTerminal, void *self, GhosttySizeReportSize *out)
{
    const auto *view = static_cast<TerminalView *>(self);
    out->rows = view->m_rowCount;
    out->columns = view->m_cols;
    out->cell_width = uint32_t(view->m_cellWidth);
    out->cell_height = uint32_t(view->m_cellHeight);
    return true;
}

void TerminalView::onTitleChanged(GhosttyTerminal terminal, void *self)
{
    auto *view = static_cast<TerminalView *>(self);
    view->m_title = ghosttyString(terminal, GHOSTTY_TERMINAL_DATA_TITLE);
    Q_EMIT view->titleChanged();
}

void TerminalView::onPwdChanged(GhosttyTerminal terminal, void *self)
{
    auto *view = static_cast<TerminalView *>(self);
    // OSC 7 sends a file:// URL, though some shells send a bare path. The
    // host is the machine name (or localhost), which toLocalFile() would
    // turn into a //host UNC path, so only the path is kept.
    const QString pwd = ghosttyString(terminal, GHOSTTY_TERMINAL_DATA_PWD);
    const QUrl url(pwd);
    view->m_shellDirectory = url.isLocalFile() ? url.path(QUrl::FullyDecoded) : pwd;
    Q_EMIT view->shellDirectoryChanged();
}

bool TerminalView::event(QEvent *event)
{
    // Window shortcuts (Ctrl+C copy, Ctrl+L path bar...) would otherwise
    // fire before the shell sees the key. Claim every key but the ones the
    // window keeps.
    if (event->type() == QEvent::ShortcutOverride) {
        auto *key = static_cast<QKeyEvent *>(event);
        const QKeySequence pressed(key->keyCombination());
        for (const QString &shortcut : std::as_const(m_windowShortcuts)) {
            if (QKeySequence(shortcut) == pressed)
                return QQuickPaintedItem::event(event);
        }
        event->accept();
        return true;
    }
    if (event->type() == QEvent::FocusIn || event->type() == QEvent::FocusOut)
        update();   // the cursor is drawn filled only while focused
    return QQuickPaintedItem::event(event);
}

void TerminalView::sendKey(QKeyEvent *event, GhosttyKeyAction action)
{
    const GhosttyKey key = ghosttyKey(event->key());
    const QString text = action == GHOSTTY_KEY_ACTION_RELEASE ? QString() : event->text();
    // Control characters in text() are Qt's rendering of Ctrl combinations;
    // the encoder builds those itself from the key and modifiers.
    const bool printable = !text.isEmpty() && text.at(0).isPrint();
    const QByteArray utf8 = printable ? text.toUtf8() : QByteArray();

    if (key == GHOSTTY_KEY_UNIDENTIFIED) {
        // Keys Ghostty has no code for (accented letters, ü on a German
        // layout) still type their text.
        if (!utf8.isEmpty())
            writePty(utf8.constData(), utf8.size());
        return;
    }

    const GhosttyMods mods = ghosttyMods(event->modifiers());
    uint32_t unshifted = 0;
    if (event->key() >= 0x20 && event->key() < 0x7f)
        unshifted = QChar(event->key()).toLower().unicode();

    ghostty_key_encoder_setopt_from_terminal(m_keyEncoder, m_terminal);
    ghostty_key_event_set_action(m_keyEvent, action);
    ghostty_key_event_set_key(m_keyEvent, key);
    ghostty_key_event_set_mods(m_keyEvent, mods);
    ghostty_key_event_set_unshifted_codepoint(m_keyEvent, unshifted);
    ghostty_key_event_set_consumed_mods(m_keyEvent,
        unshifted && !utf8.isEmpty() ? (mods & GHOSTTY_MODS_SHIFT) : GhosttyMods(0));
    ghostty_key_event_set_utf8(m_keyEvent, utf8.isEmpty() ? nullptr : utf8.constData(), size_t(utf8.size()));

    char out[128];
    size_t written = 0;
    if (ghostty_key_encoder_encode(m_keyEncoder, m_keyEvent, out, sizeof out, &written) == GHOSTTY_SUCCESS
        && written > 0) {
        writePty(out, qsizetype(written));
    } else if (!utf8.isEmpty()) {
        writePty(utf8.constData(), utf8.size());
    }
}

void TerminalView::keyPressEvent(QKeyEvent *event)
{
    if (m_ptyFd < 0) {
        event->ignore();
        return;
    }
    // Terminal copy and paste keys: Ctrl+C and Ctrl+V belong to the shell.
    const QKeyCombination combo = event->keyCombination();
    if (combo == QKeyCombination(Qt::ControlModifier | Qt::ShiftModifier, Qt::Key_C)) {
        copy();
        event->accept();
        return;
    }
    if (combo == QKeyCombination(Qt::ControlModifier | Qt::ShiftModifier, Qt::Key_V)
        || combo == QKeyCombination(Qt::ShiftModifier, Qt::Key_Insert)) {
        paste();
        event->accept();
        return;
    }
    if (m_hasSelection)
        clearSelection();
    // Typing jumps back to the prompt from wherever scrollback was.
    ghostty_terminal_scroll_viewport(m_terminal, GhosttyTerminalScrollViewport{
        GHOSTTY_SCROLL_VIEWPORT_BOTTOM, {}});
    sendKey(event, event->isAutoRepeat() ? GHOSTTY_KEY_ACTION_REPEAT : GHOSTTY_KEY_ACTION_PRESS);
    event->accept();
    update();
}

void TerminalView::keyReleaseEvent(QKeyEvent *event)
{
    // Only the kitty keyboard protocol reports releases; the encoder writes
    // nothing for them otherwise.
    if (m_ptyFd >= 0 && !event->isAutoRepeat())
        sendKey(event, GHOSTTY_KEY_ACTION_RELEASE);
    event->accept();
}

void TerminalView::inputMethodEvent(QInputMethodEvent *event)
{
    // fcitx/ibus hand over composed text here (CJK, compose sequences).
    if (!event->commitString().isEmpty())
        sendText(event->commitString());
    event->accept();
}

QVariant TerminalView::inputMethodQuery(Qt::InputMethodQuery query) const
{
    if (query == Qt::ImCursorRectangle) {
        uint16_t x = 0, y = 0;
        ghostty_terminal_get(m_terminal, GHOSTTY_TERMINAL_DATA_CURSOR_X, &x);
        ghostty_terminal_get(m_terminal, GHOSTTY_TERMINAL_DATA_CURSOR_Y, &y);
        return QRectF(Padding + x * m_cellWidth, Padding + y * m_cellHeight, m_cellWidth, m_cellHeight);
    }
    if (query == Qt::ImEnabled)
        return true;
    return QQuickPaintedItem::inputMethodQuery(query);
}

GhosttyGridRef TerminalView::refAt(QPointF pos) const
{
    GhosttyPoint point{};
    point.tag = GHOSTTY_POINT_TAG_VIEWPORT;
    point.value.coordinate.x = uint16_t(qBound(0, int((pos.x() - Padding) / m_cellWidth), m_cols - 1));
    point.value.coordinate.y = uint32_t(qBound(0, int((pos.y() - Padding) / m_cellHeight), m_rowCount - 1));
    GhosttyGridRef ref = GHOSTTY_INIT_SIZED(GhosttyGridRef);
    ghostty_terminal_grid_ref(m_terminal, point, &ref);
    return ref;
}

// Runs one press, drag or release through Ghostty's gesture state machine
// (cell drag, word on double click, line on triple) and installs the
// selection it yields. Returns whether there is one.
bool TerminalView::selectionGesture(GhosttySelectionGestureEvent event, QMouseEvent *mouse)
{
    const GhosttyGridRef ref = refAt(mouse->position());
    const GhosttySurfacePosition position{mouse->position().x(), mouse->position().y()};
    ghostty_selection_gesture_event_set(event, GHOSTTY_SELECTION_GESTURE_EVENT_OPT_REF, &ref);
    if (event != m_releaseEvent)
        ghostty_selection_gesture_event_set(event, GHOSTTY_SELECTION_GESTURE_EVENT_OPT_POSITION, &position);
    if (event == m_pressEvent) {
        const uint64_t time = uint64_t(mouse->timestamp()) * 1000000;
        const uint64_t interval = uint64_t(QGuiApplication::styleHints()->mouseDoubleClickInterval()) * 1000000;
        const double distance = QGuiApplication::styleHints()->mouseDoubleClickDistance();
        ghostty_selection_gesture_event_set(event, GHOSTTY_SELECTION_GESTURE_EVENT_OPT_TIME_NS, &time);
        ghostty_selection_gesture_event_set(event, GHOSTTY_SELECTION_GESTURE_EVENT_OPT_REPEAT_INTERVAL_NS, &interval);
        ghostty_selection_gesture_event_set(event, GHOSTTY_SELECTION_GESTURE_EVENT_OPT_REPEAT_DISTANCE, &distance);
    }
    if (event == m_dragEvent) {
        const GhosttySelectionGestureGeometry geometry{m_cols, uint32_t(m_cellWidth), uint32_t(Padding),
                                                       uint32_t(qMax(1.0, height()))};
        ghostty_selection_gesture_event_set(event, GHOSTTY_SELECTION_GESTURE_EVENT_OPT_GEOMETRY, &geometry);
        const bool rectangle = mouse->modifiers() & Qt::AltModifier;
        ghostty_selection_gesture_event_set(event, GHOSTTY_SELECTION_GESTURE_EVENT_OPT_RECTANGLE, &rectangle);
    }

    GhosttySelection selection = GHOSTTY_INIT_SIZED(GhosttySelection);
    const GhosttyResult result = ghostty_selection_gesture_event(m_gesture, m_terminal, event, &selection);
    if (event == m_releaseEvent)
        return m_hasSelection;
    if (result == GHOSTTY_SUCCESS) {
        ghostty_terminal_set(m_terminal, GHOSTTY_TERMINAL_OPT_SELECTION, &selection);
        m_hasSelection = true;
    } else if (event == m_pressEvent) {
        clearSelection();
    }
    update();
    return m_hasSelection;
}

void TerminalView::clearSelection()
{
    ghostty_terminal_set(m_terminal, GHOSTTY_TERMINAL_OPT_SELECTION, nullptr);
    m_hasSelection = false;
    update();
}

QString TerminalView::selectedText() const
{
    if (!m_hasSelection)
        return {};
    // Plain text with soft wraps joined and trailing blanks trimmed, as
    // Ghostty itself copies.
    GhosttyTerminalSelectionFormatOptions options = GHOSTTY_INIT_SIZED(GhosttyTerminalSelectionFormatOptions);
    options.emit = GHOSTTY_FORMATTER_FORMAT_PLAIN;
    options.unwrap = true;
    options.trim = true;
    uint8_t *buf = nullptr;
    size_t len = 0;
    if (ghostty_terminal_selection_format_alloc(m_terminal, nullptr, options, &buf, &len) != GHOSTTY_SUCCESS)
        return {};
    const QString text = QString::fromUtf8(reinterpret_cast<const char *>(buf), qsizetype(len));
    ghostty_free(nullptr, buf, len);
    return text;
}

void TerminalView::copy()
{
    const QString text = selectedText();
    if (!text.isEmpty())
        QGuiApplication::clipboard()->setText(text);
}

void TerminalView::paste(bool primarySelection)
{
    if (m_ptyFd < 0)
        return;
    QByteArray data = QGuiApplication::clipboard()
                          ->text(primarySelection ? QClipboard::Selection : QClipboard::Clipboard)
                          .toUtf8();
    if (data.isEmpty())
        return;
    // Ghostty's encoder strips control bytes (ESC included, so a pasted
    // end-of-paste marker cannot break out) and brackets the paste when the
    // shell turned that on, so a pasted newline does not run anything.
    GhosttyTerminalModeConfig bracketed{GHOSTTY_MODE_BRACKETED_PASTE, false};
    ghostty_terminal_get(m_terminal, GHOSTTY_TERMINAL_DATA_MODE, &bracketed);
    QByteArray out(data.size() + 16, Qt::Uninitialized);
    size_t written = 0;
    if (ghostty_paste_encode(data.data(), size_t(data.size()), bracketed.value, out.data(),
                             size_t(out.size()), &written) != GHOSTTY_SUCCESS)
        return;
    ghostty_terminal_scroll_viewport(m_terminal, GhosttyTerminalScrollViewport{
        GHOSTTY_SCROLL_VIEWPORT_BOTTOM, {}});
    writePty(out.constData(), qsizetype(written));
}

void TerminalView::mousePressEvent(QMouseEvent *event)
{
    forceActiveFocus(Qt::MouseFocusReason);
    if (event->button() == Qt::LeftButton)
        selectionGesture(m_pressEvent, event);
    else if (event->button() == Qt::MiddleButton)
        paste(true);
    event->accept();
}

void TerminalView::mouseMoveEvent(QMouseEvent *event)
{
    if (event->buttons() & Qt::LeftButton)
        selectionGesture(m_dragEvent, event);
    event->accept();
}

void TerminalView::mouseReleaseEvent(QMouseEvent *event)
{
    // A finished selection becomes the primary selection, for middle click
    // here and in other apps.
    if (event->button() == Qt::LeftButton && selectionGesture(m_releaseEvent, event)) {
        QClipboard *clipboard = QGuiApplication::clipboard();
        if (clipboard->supportsSelection())
            clipboard->setText(selectedText(), QClipboard::Selection);
    }
    event->accept();
}

void TerminalView::wheelEvent(QWheelEvent *event)
{
    // Three lines a notch, like most terminals; touchpads send fractions.
    m_scrollRemainder += event->angleDelta().y() / 120.0 * 3;
    const int lines = int(m_scrollRemainder);
    m_scrollRemainder -= lines;
    if (lines != 0) {
        GhosttyTerminalScrollViewport scroll{GHOSTTY_SCROLL_VIEWPORT_DELTA, {}};
        scroll.value.delta = -lines;
        ghostty_terminal_scroll_viewport(m_terminal, scroll);
        update();
    }
    event->accept();
}

void TerminalView::paint(QPainter *painter)
{
    if (ghostty_render_state_update(m_renderState, m_terminal) != GHOSTTY_SUCCESS)
        return;
    GhosttyRenderStateColors colors = GHOSTTY_INIT_SIZED(GhosttyRenderStateColors);
    ghostty_render_state_get(m_renderState, GHOSTTY_RENDER_STATE_DATA_COLORS, &colors);
    painter->fillRect(boundingRect(), qcolor(colors.background));
    if (ghostty_render_state_get(m_renderState, GHOSTTY_RENDER_STATE_DATA_ROW_ITERATOR, &m_rows)
        != GHOSTTY_SUCCESS)
        return;

    QFont bold = m_font;
    bold.setBold(true);
    QFont italic = m_font;
    italic.setItalic(true);
    QFont boldItalic = bold;
    boldItalic.setItalic(true);

    qreal y = Padding;
    while (ghostty_render_state_row_iterator_next(m_rows)) {
        if (ghostty_render_state_row_get(m_rows, GHOSTTY_RENDER_STATE_ROW_DATA_CELLS, &m_cells)
            != GHOSTTY_SUCCESS) {
            y += m_cellHeight;
            continue;
        }
        GhosttyRenderStateRowSelection selected = GHOSTTY_INIT_SIZED(GhosttyRenderStateRowSelection);
        const bool rowSelected = ghostty_render_state_row_get(m_rows, GHOSTTY_RENDER_STATE_ROW_DATA_SELECTION,
                                                              &selected) == GHOSTTY_SUCCESS;
        int column = 0;
        qreal x = Padding;
        while (ghostty_render_state_row_cells_next(m_cells)) {
            uint32_t graphemeLen = 0;
            ghostty_render_state_row_cells_get(m_cells, GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_GRAPHEMES_LEN,
                                               &graphemeLen);
            GhosttyStyle style = GHOSTTY_INIT_SIZED(GhosttyStyle);
            ghostty_render_state_row_cells_get(m_cells, GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_STYLE, &style);
            GhosttyColorRgb fg = colors.foreground;
            ghostty_render_state_row_cells_get(m_cells, GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_FG_COLOR, &fg);
            GhosttyColorRgb bg = colors.background;
            bool hasBg = ghostty_render_state_row_cells_get(
                             m_cells, GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_BG_COLOR, &bg) == GHOSTTY_SUCCESS;
            // Selected cells are drawn inverted, so the selection shows in
            // any theme and over any colours the program chose.
            const bool cellSelected = rowSelected && column >= selected.start_x && column <= selected.end_x;
            if (style.inverse != cellSelected) {
                std::swap(fg, bg);
                hasBg = true;
            }

            // A wide character draws across its own cell and the spacer after it.
            GhosttyCell raw = 0;
            GhosttyCellWide wide = GHOSTTY_CELL_WIDE_NARROW;
            if (ghostty_render_state_row_cells_get(m_cells, GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_RAW, &raw)
                == GHOSTTY_SUCCESS)
                ghostty_cell_get(raw, GHOSTTY_CELL_DATA_WIDE, &wide);
            const qreal cellWidth = wide == GHOSTTY_CELL_WIDE_WIDE ? 2 * m_cellWidth : m_cellWidth;

            if (hasBg)
                painter->fillRect(QRectF(x, y, cellWidth, m_cellHeight), qcolor(bg));

            if (graphemeLen > 0 && !style.invisible) {
                // The shell decides the length (any number of combining
                // marks), so the buffer is sized to it, not fixed.
                QVarLengthArray<uint32_t, 16> codepoints(graphemeLen);
                ghostty_render_state_row_cells_get(m_cells, GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_GRAPHEMES_BUF,
                                                   codepoints.data());
                const QString text = QString::fromUcs4(reinterpret_cast<const char32_t *>(codepoints.data()),
                                                       codepoints.size());
                QColor color = qcolor(fg);
                if (style.faint)
                    color.setAlphaF(0.6);
                painter->setPen(color);
                painter->setFont(style.bold ? (style.italic ? boldItalic : bold)
                                            : (style.italic ? italic : m_font));
                painter->drawText(QPointF(x, y + m_ascent), text);
                if (style.underline)
                    painter->drawLine(QPointF(x, y + m_ascent + 2), QPointF(x + cellWidth, y + m_ascent + 2));
                if (style.strikethrough)
                    painter->drawLine(QPointF(x, y + m_cellHeight / 2), QPointF(x + cellWidth, y + m_cellHeight / 2));
            }
            x += m_cellWidth;
            ++column;
        }
        y += m_cellHeight;
    }

    bool cursorVisible = false;
    bool cursorInView = false;
    ghostty_render_state_get(m_renderState, GHOSTTY_RENDER_STATE_DATA_CURSOR_VISIBLE, &cursorVisible);
    ghostty_render_state_get(m_renderState, GHOSTTY_RENDER_STATE_DATA_CURSOR_VIEWPORT_HAS_VALUE, &cursorInView);
    if (cursorVisible && cursorInView) {
        uint16_t cx = 0, cy = 0;
        ghostty_render_state_get(m_renderState, GHOSTTY_RENDER_STATE_DATA_CURSOR_VIEWPORT_X, &cx);
        ghostty_render_state_get(m_renderState, GHOSTTY_RENDER_STATE_DATA_CURSOR_VIEWPORT_Y, &cy);
        QColor cursor = colors.cursor_has_value ? qcolor(colors.cursor) : qcolor(colors.foreground);
        const QRectF cell(Padding + cx * m_cellWidth, Padding + cy * m_cellHeight, m_cellWidth, m_cellHeight);
        if (hasActiveFocus()) {
            cursor.setAlphaF(0.5);
            painter->fillRect(cell, cursor);
        } else {
            painter->setPen(cursor);
            painter->drawRect(cell.adjusted(0.5, 0.5, -0.5, -0.5));
        }
    }
}
