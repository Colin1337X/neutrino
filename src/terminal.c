#include "neutrino.h"
#include <locale.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
static HANDLE input, output;
static DWORD old_in, old_out;
static bool active;
static SHORT last_width, last_height;

bool terminal_open(void)
{
    input = GetStdHandle(STD_INPUT_HANDLE);
    output = GetStdHandle(STD_OUTPUT_HANDLE);
    if (!GetConsoleMode(input, &old_in) || !GetConsoleMode(output, &old_out)) return false;
    if (!SetConsoleMode(output, old_out | ENABLE_VIRTUAL_TERMINAL_PROCESSING)) return false;
    if (!SetConsoleMode(input, ENABLE_WINDOW_INPUT | ENABLE_EXTENDED_FLAGS)) {
        SetConsoleMode(output, old_out);
        return false;
    }
    return active = true;
}
void terminal_close(void)
{
    if (!active) return;
    SetConsoleMode(input, old_in);
    SetConsoleMode(output, old_out);
    active = false;
}
void terminal_write(const char *data, size_t n)
{
    if (!n) return;
    int size = MultiByteToWideChar(CP_UTF8, 0, data, (int)n, NULL, 0);
    static wchar_t buf[262144];
    if (size <= 0 || size > (int)(sizeof buf / sizeof *buf)) return;
    MultiByteToWideChar(CP_UTF8, 0, data, (int)n, buf, size);
    DWORD offset = 0;
    while (offset < (DWORD)size) {
        DWORD count = (DWORD)size - offset, written;
        if (count > 16000) count = 16000;
        if (offset + count < (DWORD)size &&
            buf[offset + count - 1] >= 0xd800 && buf[offset + count - 1] <= 0xdbff) count--;
        if (!WriteConsoleW(output, buf + offset, count, &written, NULL) || !written) break;
        offset += written;
    }
}
void terminal_size(int *cols, int *rows)
{
    CONSOLE_SCREEN_BUFFER_INFO info;
    if (GetConsoleScreenBufferInfo(output, &info)) {
        last_width = info.srWindow.Right - info.srWindow.Left + 1;
        last_height = info.srWindow.Bottom - info.srWindow.Top + 1;
    }
    *cols = last_width ? last_width : 80;
    *rows = last_height ? last_height : 24;
    bound_terminal_viewport(cols, rows);
}
static int decode_key(KEY_EVENT_RECORD key)
{
    static int lead;
    const bool shift = (key.dwControlKeyState & SHIFT_PRESSED) != 0;
    const bool ctrl = (key.dwControlKeyState & (LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED)) != 0;
    if (ctrl && key.wVirtualKeyCode == VK_SPACE) return KEY_MARK;
    if (ctrl && key.wVirtualKeyCode == 'P') return KEY_FOCUS;
    switch (key.wVirtualKeyCode) {
        case VK_UP: return KEY_UP;
        case VK_DOWN: return KEY_DOWN;
        case VK_LEFT: return KEY_LEFT;
        case VK_RIGHT: return KEY_RIGHT;
        case VK_HOME: return KEY_HOME;
        case VK_END: return KEY_END;
        case VK_PRIOR: return KEY_PGUP;
        case VK_NEXT: return KEY_PGDN;
        case VK_DELETE: return KEY_DEL;
        case VK_TAB: return shift ? KEY_BACKTAB : 9;
    }
    if (key.wVirtualKeyCode >= VK_F1 && key.wVirtualKeyCode <= VK_F12)
        return KEY_F1 + key.wVirtualKeyCode - VK_F1;
    int cp = key.uChar.UnicodeChar;
    if (cp >= 0xd800 && cp <= 0xdbff) { lead = cp; return 0; }
    if (cp >= 0xdc00 && cp <= 0xdfff)
        cp = lead ? 0x10000 + ((lead - 0xd800) << 10) + cp - 0xdc00 : 0xfffd;
    lead = 0;
    return cp;
}
int terminal_key(int timeout)
{
    static int repeat, previous;
    if (repeat > 0) { repeat--; return previous; }
    DWORD ready;
    while ((ready = WaitForSingleObject(input, (DWORD)timeout)) == WAIT_OBJECT_0) {
        INPUT_RECORD evt;
        DWORD n;
        if (!ReadConsoleInputW(input, &evt, 1, &n) || !n) return KEY_CLOSED;
        if (evt.EventType == WINDOW_BUFFER_SIZE_EVENT) return KEY_RESIZE;
        if (evt.EventType == KEY_EVENT && evt.Event.KeyEvent.bKeyDown) {
            int key = decode_key(evt.Event.KeyEvent);
            if (key) {
                repeat = evt.Event.KeyEvent.wRepeatCount - 1;
                return previous = key;
            }
        }
        timeout = 0;
    }
    return ready == WAIT_FAILED ? KEY_CLOSED : -1;
}
#else
#include <errno.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>
static struct termios saved;
static bool active;
static bool paste;
static int pushed_byte = -1;

bool terminal_open(void)
{
    if (!isatty(STDOUT_FILENO) || tcgetattr(STDIN_FILENO, &saved)) return false;
    struct termios options = saved;
    options.c_lflag &= (tcflag_t)~(ICANON | ECHO | ISIG | IEXTEN);
    options.c_iflag &= (tcflag_t)~(IXON | ICRNL | ISTRIP);
    options.c_cc[VMIN] = 1;
    options.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &options)) return false;
    setlocale(LC_CTYPE, "");
    terminal_write("\x1b[?2004h", sizeof "\x1b[?2004h" - 1);
    return active = true;
}
void terminal_close(void)
{
    if (!active) return;
    terminal_write("\x1b[?2004l", sizeof "\x1b[?2004l" - 1);
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &saved);
    active = false;
}
void terminal_write(const char *data, size_t n)
{
    while (n) {
        ssize_t done = write(STDOUT_FILENO, data, n);
        if (done < 0 && errno == EINTR) continue;
        if (done <= 0) return;
        data += done;
        n -= (size_t)done;
    }
}
void terminal_size(int *cols, int *rows)
{
    struct winsize size = {0};
    ioctl(STDOUT_FILENO, TIOCGWINSZ, &size);
    *cols = size.ws_col ? size.ws_col : 80;
    *rows = size.ws_row ? size.ws_row : 24;
    bound_terminal_viewport(cols, rows);
}
static int byte_key(int timeout)
{
    static unsigned char buf[1024];
    static size_t offset, size;
    if (pushed_byte >= 0) {
        int byte = pushed_byte;
        pushed_byte = -1;
        return byte;
    }
    if (offset < size) return buf[offset++];
    struct pollfd fd = {STDIN_FILENO, POLLIN, 0};
    if (poll(&fd, 1, timeout) <= 0) return -1;
    if (!(fd.revents & POLLIN)) return -2;
    ssize_t n = read(STDIN_FILENO, buf, sizeof buf);
    if (n <= 0) return n < 0 && errno == EINTR ? -1 : -2;
    size = (size_t)n;
    offset = 1;
    return buf[0];
}
static int escape_key(void)
{
    int next = byte_key(25);
    if (next != '[' && next != 'O') {
        pushed_byte = next;
        return paste ? -1 : 27;
    }
    char seq[20] = {0};
    size_t n = 0;
    while (n < sizeof seq - 1) {
        int b = byte_key(25);
        if (b < 0) break;
        seq[n++] = (char)b;
        if (b >= 0x40 && b <= 0x7e) break;
    }
    if (!strcmp(seq, "200~")) { paste = true; return -1; }
    if (!strcmp(seq, "201~")) { paste = false; return -1; }
    if (paste) return -1;
    const struct { const char *seq; int key; } table[] = {
        {"A", KEY_UP}, {"B", KEY_DOWN}, {"C", KEY_RIGHT}, {"D", KEY_LEFT},
        {"H", KEY_HOME}, {"1~", KEY_HOME}, {"F", KEY_END}, {"4~", KEY_END},
        {"3~", KEY_DEL}, {"5~", KEY_PGUP}, {"6~", KEY_PGDN}, {"Z", KEY_BACKTAB},
        {"P", KEY_F1}, {"Q", KEY_F2}, {"R", KEY_F3}, {"S", KEY_F4},
        {"11~", KEY_F1}, {"12~", KEY_F2}, {"13~", KEY_F3}, {"14~", KEY_F4},
        {"15~", KEY_F5}, {"17~", KEY_F6}, {"18~", KEY_F7}, {"19~", KEY_F8},
        {"20~", KEY_F9}, {"21~", KEY_F10}, {"23~", KEY_F11}, {"24~", KEY_F12}
    };
    for (size_t i = 0; i < sizeof table / sizeof *table; i++)
        if (!strcmp(seq, table[i].seq)) return table[i].key;
    return -1;
}
int terminal_key(int timeout)
{
    int b = byte_key(timeout);
    if (b == -2) return KEY_CLOSED;
    if (b < 0) return -1;
    if (b == 27) return escape_key();
    if (paste && b < 32) return b == 10 ? '\v' : b == 9 ? ' ' : -1;
    if (b == 0) return KEY_MARK;
    if (b == 16) return KEY_FOCUS;
    if (b < 128) return b;
    int n = b >= 0xf0 ? 3 : b >= 0xe0 ? 2 : b >= 0xc2 ? 1 : 0;
    int cp = b & (0x7f >> (n + 1));
    if (!n || b > 0xf4) return 0xfffd;
    for (int i = 0; i < n; i++) {
        b = byte_key(25);
        if (b < 0 || (b & 0xc0) != 0x80) return 0xfffd;
        cp = (cp << 6) | (b & 0x3f);
    }
    int floor = n == 1 ? 0x80 : n == 2 ? 0x800 : 0x10000;
    return cp >= floor && cp <= 0x10ffff && !(cp >= 0xd800 && cp <= 0xdfff) ? cp : 0xfffd;
}
#endif

size_t utf_encode(int cp, char bytes[4])
{
    if (cp < 0x80) { bytes[0] = (char)cp; return 1; }
    if (cp < 0x800) {
        bytes[0] = (char)(0xc0 | (cp >> 6));
        bytes[1] = (char)(0x80 | (cp & 63));
        return 2;
    }
    size_t n = cp < 0x10000 ? 3u : 4u;
    for (size_t i = n - 1; i > 0; i--, cp >>= 6)
        bytes[i] = (char)(0x80 | (cp & 63));
    bytes[0] = (char)((n == 3 ? 0xe0 : 0xf0) | cp);
    return n;
}
size_t utf_next(const char *s, size_t length, size_t pos)
{
    if (pos < length) pos++;
    while (pos < length && ((unsigned char)s[pos] & 0xc0) == 0x80) pos++;
    return pos;
}
size_t utf_prev(const char *s, size_t pos)
{
    if (pos) pos--;
    while (pos && ((unsigned char)s[pos] & 0xc0) == 0x80) pos--;
    return pos;
}
int utf_width(const char *s, size_t n)
{
    unsigned cp = (unsigned char)*s;
    if (cp < 0x20 || cp == 0x7f) return cp == 9 ? 4 : 1;
    if (cp < 0x80) return 1;
    cp = cp & (n == 2 ? 31u : n == 3 ? 15u : 7u);
    if (n < 2 || n > 4) return 1;
    for (size_t i = 1; i < n; i++) cp = (cp << 6) | ((unsigned char)s[i] & 63u);
    if (cp < 0xa0) return 1;
#ifndef _WIN32
    int w = wcwidth((wchar_t)cp);
    if (w >= 0) return w;
#endif
    if ((cp >= 0x300 && cp <= 0x36f) || cp == 0x200d ||
        (cp >= 0xfe00 && cp <= 0xfe0f)) return 0;
    if ((cp >= 0x1100 && cp <= 0x115f) || (cp >= 0x2e80 && cp <= 0xa4cf) ||
        (cp >= 0xac00 && cp <= 0xd7a3) || (cp >= 0xf900 && cp <= 0xfaff) ||
        (cp >= 0xff01 && cp <= 0xff60) || (cp >= 0x1f300 && cp <= 0x1faff) ||
        (cp >= 0x20000 && cp <= 0x3fffd)) return 2;
    return 1;
}
