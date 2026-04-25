// TUI-grade Console singleton: styled output, cursor/screen control, raw input.
// Hand-rolled (terminal escape sequences + termios/WinConsole), no Godot deps.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cctype>
#include <clocale>

#include "core/variant/variant.h"
#include "natives.hpp"

#ifdef _WIN32
    #include <windows.h>
    #include <conio.h>
    #include <io.h>
    #define ZYM_ISATTY(fd) _isatty(fd)
    #define ZYM_FILENO(f)  _fileno(f)
#else
    #include <unistd.h>
    #include <termios.h>
    #include <sys/ioctl.h>
    #include <sys/select.h>
    #define ZYM_ISATTY(fd) isatty(fd)
    #define ZYM_FILENO(f)  fileno(f)
#endif

namespace {

struct ConsoleData {
    FILE* out;
    FILE* in;
    FILE* err;

    int  foreground;       // -1 = default, 0..15 ANSI
    int  background;
    bool use_rgb_fg;
    bool use_rgb_bg;
    int  fg_r, fg_g, fg_b;
    int  bg_r, bg_g, bg_b;

    bool bold;
    bool dim;
    bool italic;
    bool underline;
    bool reverse;
    bool strikethrough;

    int  cursor_x;
    int  cursor_y;
    bool cursor_visible;

    int  width;
    int  height;

    bool raw_mode;
    bool alt_screen;

#ifdef _WIN32
    HANDLE hOut;
    HANDLE hIn;
    DWORD  originalOutMode;
    DWORD  originalInMode;
    UINT   originalOutCP;
    bool   inMode_saved;
#else
    struct termios original_termios;
    bool   termios_saved;
#endif
};

// ---- helpers -------------------------------------------------------------

static void getConsoleSize(ConsoleData* c) {
#ifdef _WIN32
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    if (GetConsoleScreenBufferInfo(c->hOut, &csbi)) {
        c->width  = csbi.srWindow.Right  - csbi.srWindow.Left + 1;
        c->height = csbi.srWindow.Bottom - csbi.srWindow.Top  + 1;
    } else {
        c->width = 80; c->height = 24;
    }
#else
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0) {
        c->width = ws.ws_col; c->height = ws.ws_row;
    } else {
        c->width = 80; c->height = 24;
    }
#endif
}

static const char* fgCode(int color) {
    static const char* codes[] = {
        "30","31","32","33","34","35","36","37",
        "90","91","92","93","94","95","96","97"
    };
    if (color >= 0 && color < 16) return codes[color];
    return "39";
}

static const char* bgCode(int color) {
    static const char* codes[] = {
        "40","41","42","43","44","45","46","47",
        "100","101","102","103","104","105","106","107"
    };
    if (color >= 0 && color < 16) return codes[color];
    return "49";
}

// Maps a string color name to 0..15, or -1 if unknown.
static int colorFromName(const char* n) {
    if (!n) return -1;
    if (!strcmp(n,"black"))         return 0;
    if (!strcmp(n,"red"))           return 1;
    if (!strcmp(n,"green"))         return 2;
    if (!strcmp(n,"yellow"))        return 3;
    if (!strcmp(n,"blue"))          return 4;
    if (!strcmp(n,"magenta"))       return 5;
    if (!strcmp(n,"cyan"))          return 6;
    if (!strcmp(n,"white"))         return 7;
    if (!strcmp(n,"gray") ||
        !strcmp(n,"brightBlack") ||
        !strcmp(n,"bright_black"))  return 8;
    if (!strcmp(n,"brightRed")   || !strcmp(n,"bright_red"))     return 9;
    if (!strcmp(n,"brightGreen") || !strcmp(n,"bright_green"))   return 10;
    if (!strcmp(n,"brightYellow")|| !strcmp(n,"bright_yellow"))  return 11;
    if (!strcmp(n,"brightBlue")  || !strcmp(n,"bright_blue"))    return 12;
    if (!strcmp(n,"brightMagenta")||!strcmp(n,"bright_magenta")) return 13;
    if (!strcmp(n,"brightCyan")  || !strcmp(n,"bright_cyan"))    return 14;
    if (!strcmp(n,"brightWhite") || !strcmp(n,"bright_white"))   return 15;
    return -1;
}

static void applyStyles(ConsoleData* c) {
    fprintf(c->out, "\033[0m");
    if (c->bold)          fprintf(c->out, "\033[1m");
    if (c->dim)           fprintf(c->out, "\033[2m");
    if (c->italic)        fprintf(c->out, "\033[3m");
    if (c->underline)     fprintf(c->out, "\033[4m");
    if (c->reverse)       fprintf(c->out, "\033[7m");
    if (c->strikethrough) fprintf(c->out, "\033[9m");
    if (c->use_rgb_fg)
        fprintf(c->out, "\033[38;2;%d;%d;%dm", c->fg_r, c->fg_g, c->fg_b);
    else if (c->foreground >= 0)
        fprintf(c->out, "\033[%sm", fgCode(c->foreground));
    if (c->use_rgb_bg)
        fprintf(c->out, "\033[48;2;%d;%d;%dm", c->bg_r, c->bg_g, c->bg_b);
    else if (c->background >= 0)
        fprintf(c->out, "\033[%sm", bgCode(c->background));
}

static ConsoleData* unwrap(ZymValue ctx) {
    return static_cast<ConsoleData*>(zym_getNativeData(ctx));
}

static bool reqBool(ZymVM* vm, ZymValue v, const char* where, bool* out) {
    if (!zym_isBool(v)) { zym_runtimeError(vm, "%s expects a bool", where); return false; }
    *out = zym_asBool(v); return true;
}
static bool reqStr(ZymVM* vm, ZymValue v, const char* where, const char** out) {
    if (!zym_isString(v)) { zym_runtimeError(vm, "%s expects a string", where); return false; }
    *out = zym_asCString(v); return true;
}
static bool reqInt(ZymVM* vm, ZymValue v, const char* where, int64_t* out) {
    if (!zym_isNumber(v)) { zym_runtimeError(vm, "%s expects a number", where); return false; }
    *out = (int64_t)zym_asNumber(v); return true;
}

// Optional integer count (default = dflt) read from a 1-arg variadic tail.
static bool readOptCount(ZymVM* vm, ZymValue* vargs, int vargc, const char* where,
                         int dflt, int* out) {
    if (vargc <= 0) { *out = dflt; return true; }
    if (!zym_isNumber(vargs[0])) {
        zym_runtimeError(vm, "%s expects a number count", where);
        return false;
    }
    *out = (int)zym_asNumber(vargs[0]);
    return true;
}

// Resolve a Buffer arg (map with __pba__ context) -> PackedByteArray*.
static bool reqBufferArg(ZymVM* vm, ZymValue v, const char* where, PackedByteArray** out) {
    if (zym_isMap(v)) {
        ZymValue ctx = zym_mapGet(vm, v, "__pba__");
        if (ctx != ZYM_ERROR) {
            void* d = zym_getNativeData(ctx);
            if (d) { *out = static_cast<PackedByteArray*>(d); return true; }
        }
    }
    zym_runtimeError(vm, "%s expects a Buffer", where);
    return false;
}

// Set fg or bg color from a number (0..15) or string name.
static bool setColorFromValue(ZymVM* vm, ZymValue v, const char* where,
                              ConsoleData* c, bool isBackground) {
    if (zym_isNumber(v)) {
        int n = (int)zym_asNumber(v);
        if (n < 0 || n > 15) { zym_runtimeError(vm, "%s: color must be 0..15", where); return false; }
        if (isBackground) { c->background = n; c->use_rgb_bg = false; }
        else              { c->foreground = n; c->use_rgb_fg = false; }
        return true;
    }
    if (zym_isString(v)) {
        int n = colorFromName(zym_asCString(v));
        if (n < 0) { zym_runtimeError(vm, "%s: unknown color name", where); return false; }
        if (isBackground) { c->background = n; c->use_rgb_bg = false; }
        else              { c->foreground = n; c->use_rgb_fg = false; }
        return true;
    }
    zym_runtimeError(vm, "%s: color must be a number or string", where);
    return false;
}

}  // namespace

// ---- finalizer -----------------------------------------------------------

static void consoleFinalizer(ZymVM*, void* ptr) {
    ConsoleData* c = static_cast<ConsoleData*>(ptr);
    if (!c) return;
    fprintf(c->out, "\033[0m");
    if (!c->cursor_visible) fprintf(c->out, "\033[?25h");
    if (c->alt_screen)      fprintf(c->out, "\033[?1049l");
#ifdef _WIN32
    if (c->hOut)            SetConsoleMode(c->hOut, c->originalOutMode);
    if (c->inMode_saved)    SetConsoleMode(c->hIn,  c->originalInMode);
    if (c->originalOutCP)   SetConsoleOutputCP(c->originalOutCP);
#else
    if (c->termios_saved)   tcsetattr(STDIN_FILENO, TCSANOW, &c->original_termios);
#endif
    fflush(c->out);
    delete c;
}

// ---- methods (write / flush) --------------------------------------------

static ZymValue c_write(ZymVM* vm, ZymValue ctx, ZymValue tv) {
    ConsoleData* c = unwrap(ctx);
    const char* s; if (!reqStr(vm, tv, "Console.write(text)", &s)) return ZYM_ERROR;
    fputs(s, c->out);
    return zym_newNull();
}
static ZymValue c_writeLine(ZymVM* vm, ZymValue ctx, ZymValue tv) {
    ConsoleData* c = unwrap(ctx);
    const char* s; if (!reqStr(vm, tv, "Console.writeLine(text)", &s)) return ZYM_ERROR;
    fputs(s, c->out); fputc('\n', c->out);
    return zym_newNull();
}
static ZymValue c_writeErr(ZymVM* vm, ZymValue ctx, ZymValue tv) {
    ConsoleData* c = unwrap(ctx);
    const char* s; if (!reqStr(vm, tv, "Console.writeErr(text)", &s)) return ZYM_ERROR;
    fputs(s, c->err);
    return zym_newNull();
}
static ZymValue c_writeLineErr(ZymVM* vm, ZymValue ctx, ZymValue tv) {
    ConsoleData* c = unwrap(ctx);
    const char* s; if (!reqStr(vm, tv, "Console.writeLineErr(text)", &s)) return ZYM_ERROR;
    fputs(s, c->err); fputc('\n', c->err);
    return zym_newNull();
}
static ZymValue c_writeBuffer(ZymVM* vm, ZymValue ctx, ZymValue bv) {
    ConsoleData* c = unwrap(ctx);
    PackedByteArray* buf;
    if (!reqBufferArg(vm, bv, "Console.writeBuffer(buf)", &buf)) return ZYM_ERROR;
    if (buf->size() > 0) fwrite(buf->ptr(), 1, (size_t)buf->size(), c->out);
    return zym_newNull();
}
static ZymValue c_writeBufferErr(ZymVM* vm, ZymValue ctx, ZymValue bv) {
    ConsoleData* c = unwrap(ctx);
    PackedByteArray* buf;
    if (!reqBufferArg(vm, bv, "Console.writeBufferErr(buf)", &buf)) return ZYM_ERROR;
    if (buf->size() > 0) fwrite(buf->ptr(), 1, (size_t)buf->size(), c->err);
    return zym_newNull();
}
static ZymValue c_flush(ZymVM*, ZymValue ctx) {
    ConsoleData* c = unwrap(ctx);
    fflush(c->out); fflush(c->err);
    return zym_newNull();
}

// ---- methods (color / style) --------------------------------------------

static ZymValue c_setColor(ZymVM* vm, ZymValue ctx, ZymValue v) {
    ConsoleData* c = unwrap(ctx);
    if (!setColorFromValue(vm, v, "Console.setColor(color)", c, /*bg=*/false)) return ZYM_ERROR;
    applyStyles(c); return zym_newNull();
}
static ZymValue c_setBackgroundColor(ZymVM* vm, ZymValue ctx, ZymValue v) {
    ConsoleData* c = unwrap(ctx);
    if (!setColorFromValue(vm, v, "Console.setBackgroundColor(color)", c, /*bg=*/true)) return ZYM_ERROR;
    applyStyles(c); return zym_newNull();
}
static ZymValue c_setColorRGB(ZymVM* vm, ZymValue ctx, ZymValue rv, ZymValue gv, ZymValue bv) {
    ConsoleData* c = unwrap(ctx);
    int64_t r,g,b;
    if (!reqInt(vm, rv, "Console.setColorRGB(r,g,b)", &r)) return ZYM_ERROR;
    if (!reqInt(vm, gv, "Console.setColorRGB(r,g,b)", &g)) return ZYM_ERROR;
    if (!reqInt(vm, bv, "Console.setColorRGB(r,g,b)", &b)) return ZYM_ERROR;
    if (r<0||r>255||g<0||g>255||b<0||b>255) {
        zym_runtimeError(vm, "Console.setColorRGB: components must be 0..255"); return ZYM_ERROR;
    }
    c->fg_r=(int)r; c->fg_g=(int)g; c->fg_b=(int)b; c->use_rgb_fg=true;
    applyStyles(c); return zym_newNull();
}
static ZymValue c_setBackgroundColorRGB(ZymVM* vm, ZymValue ctx, ZymValue rv, ZymValue gv, ZymValue bv) {
    ConsoleData* c = unwrap(ctx);
    int64_t r,g,b;
    if (!reqInt(vm, rv, "Console.setBackgroundColorRGB(r,g,b)", &r)) return ZYM_ERROR;
    if (!reqInt(vm, gv, "Console.setBackgroundColorRGB(r,g,b)", &g)) return ZYM_ERROR;
    if (!reqInt(vm, bv, "Console.setBackgroundColorRGB(r,g,b)", &b)) return ZYM_ERROR;
    if (r<0||r>255||g<0||g>255||b<0||b>255) {
        zym_runtimeError(vm, "Console.setBackgroundColorRGB: components must be 0..255"); return ZYM_ERROR;
    }
    c->bg_r=(int)r; c->bg_g=(int)g; c->bg_b=(int)b; c->use_rgb_bg=true;
    applyStyles(c); return zym_newNull();
}
static ZymValue c_reset(ZymVM*, ZymValue ctx) {
    ConsoleData* c = unwrap(ctx);
    c->foreground=-1; c->background=-1;
    c->use_rgb_fg=false; c->use_rgb_bg=false;
    c->bold=false; c->dim=false; c->italic=false;
    c->underline=false; c->reverse=false; c->strikethrough=false;
    fprintf(c->out, "\033[0m");
    fflush(c->out);
    return zym_newNull();
}

#define STYLE_TOGGLE(METH, FIELD, WHERE) \
    static ZymValue METH(ZymVM* vm, ZymValue ctx, ZymValue v) { \
        ConsoleData* c = unwrap(ctx); bool b; \
        if (!reqBool(vm, v, WHERE, &b)) return ZYM_ERROR; \
        c->FIELD = b; applyStyles(c); return zym_newNull(); \
    }
STYLE_TOGGLE(c_setBold,          bold,          "Console.setBold(on)")
STYLE_TOGGLE(c_setDim,           dim,           "Console.setDim(on)")
STYLE_TOGGLE(c_setItalic,        italic,        "Console.setItalic(on)")
STYLE_TOGGLE(c_setUnderline,     underline,     "Console.setUnderline(on)")
STYLE_TOGGLE(c_setReverse,       reverse,       "Console.setReverse(on)")
STYLE_TOGGLE(c_setStrikethrough, strikethrough, "Console.setStrikethrough(on)")
#undef STYLE_TOGGLE

// ---- methods (cursor) ---------------------------------------------------

static ZymValue c_moveCursor(ZymVM* vm, ZymValue ctx, ZymValue rv, ZymValue cv) {
    ConsoleData* c = unwrap(ctx);
    int64_t row, col;
    if (!reqInt(vm, rv, "Console.moveCursor(row,col)", &row)) return ZYM_ERROR;
    if (!reqInt(vm, cv, "Console.moveCursor(row,col)", &col)) return ZYM_ERROR;
    if (row < 1) row = 1;
    if (col < 1) col = 1;
    fprintf(c->out, "\033[%d;%dH", (int)row, (int)col);
    c->cursor_x = (int)col - 1; c->cursor_y = (int)row - 1;
    return zym_newNull();
}

#define CURSOR_DELTA(METH, ESC, AXIS, SIGN, WHERE)                              \
    static ZymValue METH(ZymVM* vm, ZymValue ctx, ZymValue* va, int vc) {       \
        ConsoleData* c = unwrap(ctx);                                            \
        int n; if (!readOptCount(vm, va, vc, WHERE, 1, &n)) return ZYM_ERROR;    \
        if (n > 0) {                                                             \
            fprintf(c->out, "\033[%d" ESC, n);                                   \
            c->AXIS SIGN n;                                                      \
            if (c->AXIS < 0) c->AXIS = 0;                                        \
        }                                                                        \
        return zym_newNull();                                                              \
    }
CURSOR_DELTA(c_moveCursorUp,    "A", cursor_y, -=, "Console.moveCursorUp(n)")
CURSOR_DELTA(c_moveCursorDown,  "B", cursor_y, +=, "Console.moveCursorDown(n)")
CURSOR_DELTA(c_moveCursorRight, "C", cursor_x, +=, "Console.moveCursorRight(n)")
CURSOR_DELTA(c_moveCursorLeft,  "D", cursor_x, -=, "Console.moveCursorLeft(n)")
#undef CURSOR_DELTA

static ZymValue c_hideCursor(ZymVM*, ZymValue ctx) {
    ConsoleData* c = unwrap(ctx); fputs("\033[?25l", c->out); c->cursor_visible = false; return zym_newNull();
}
static ZymValue c_showCursor(ZymVM*, ZymValue ctx) {
    ConsoleData* c = unwrap(ctx); fputs("\033[?25h", c->out); c->cursor_visible = true; return zym_newNull();
}
static ZymValue c_saveCursorPos(ZymVM*, ZymValue ctx) {
    ConsoleData* c = unwrap(ctx); fputs("\033[s", c->out); return zym_newNull();
}
static ZymValue c_restoreCursorPos(ZymVM*, ZymValue ctx) {
    ConsoleData* c = unwrap(ctx); fputs("\033[u", c->out); return zym_newNull();
}

// ---- methods (clear / scroll / screen) ---------------------------------

static ZymValue c_clear(ZymVM*, ZymValue ctx) {
    ConsoleData* c = unwrap(ctx); fputs("\033[2J\033[H", c->out);
    c->cursor_x = 0; c->cursor_y = 0; return zym_newNull();
}
static ZymValue c_clearLine(ZymVM*, ZymValue ctx) {
    ConsoleData* c = unwrap(ctx); fputs("\033[2K", c->out); return zym_newNull();
}
static ZymValue c_clearToEndOfLine(ZymVM*, ZymValue ctx) {
    ConsoleData* c = unwrap(ctx); fputs("\033[K",  c->out); return zym_newNull();
}
static ZymValue c_clearToStartOfLine(ZymVM*, ZymValue ctx) {
    ConsoleData* c = unwrap(ctx); fputs("\033[1K", c->out); return zym_newNull();
}

#define SCROLL(METH, ESC, WHERE)                                                  \
    static ZymValue METH(ZymVM* vm, ZymValue ctx, ZymValue* va, int vc) {         \
        ConsoleData* c = unwrap(ctx);                                              \
        int n; if (!readOptCount(vm, va, vc, WHERE, 1, &n)) return ZYM_ERROR;      \
        if (n > 0) fprintf(c->out, "\033[%d" ESC, n);                              \
        return zym_newNull();                                                                \
    }
SCROLL(c_scrollUp,   "S", "Console.scrollUp(n)")
SCROLL(c_scrollDown, "T", "Console.scrollDown(n)")
#undef SCROLL

static ZymValue c_useAltScreen(ZymVM*, ZymValue ctx) {
    ConsoleData* c = unwrap(ctx); fputs("\033[?1049h", c->out); c->alt_screen = true;  return zym_newNull();
}
static ZymValue c_useMainScreen(ZymVM*, ZymValue ctx) {
    ConsoleData* c = unwrap(ctx); fputs("\033[?1049l", c->out); c->alt_screen = false; return zym_newNull();
}
static ZymValue c_setTitle(ZymVM* vm, ZymValue ctx, ZymValue tv) {
    ConsoleData* c = unwrap(ctx);
    const char* s; if (!reqStr(vm, tv, "Console.setTitle(text)", &s)) return ZYM_ERROR;
    fprintf(c->out, "\033]0;%s\007", s);
    return zym_newNull();
}
static ZymValue c_beep(ZymVM*, ZymValue ctx) {
    ConsoleData* c = unwrap(ctx); fputc('\a', c->out); return zym_newNull();
}

// ---- methods (input) ----------------------------------------------------

static ZymValue c_readLine(ZymVM* vm, ZymValue ctx) {
    ConsoleData* c = unwrap(ctx);
    char buf[4096];
    if (fgets(buf, sizeof(buf), c->in) == nullptr) return zym_newNull();
    size_t len = strlen(buf);
    if (len > 0 && buf[len-1] == '\n') buf[--len] = 0;
    if (len > 0 && buf[len-1] == '\r') buf[--len] = 0;
    return zym_newString(vm, buf);
}

static ZymValue c_readChar(ZymVM* vm, ZymValue ctx) {
    ConsoleData* c = unwrap(ctx);
#ifdef _WIN32
    int ch = _getch();
#else
    int ch = fgetc(c->in);
#endif
    if (ch == EOF) return zym_newNull();
    char s[2] = { (char)ch, 0 };
    return zym_newString(vm, s);
}

static ZymValue c_hasInput(ZymVM*, ZymValue ctx) {
    ConsoleData* c = unwrap(ctx);
#ifdef _WIN32
    (void)c; return zym_newBool(_kbhit() != 0);
#else
    struct timeval tv = { 0, 0 };
    fd_set fds; FD_ZERO(&fds); FD_SET(ZYM_FILENO(c->in), &fds);
    int n = select(ZYM_FILENO(c->in) + 1, &fds, nullptr, nullptr, &tv);
    return zym_newBool(n > 0);
#endif
}

static ZymValue c_setRawMode(ZymVM* vm, ZymValue ctx, ZymValue v) {
    ConsoleData* c = unwrap(ctx);
    bool enable; if (!reqBool(vm, v, "Console.setRawMode(on)", &enable)) return ZYM_ERROR;
#ifdef _WIN32
    if (enable) {
        if (!c->inMode_saved) {
            GetConsoleMode(c->hIn, &c->originalInMode);
            c->inMode_saved = true;
        }
        DWORD m = c->originalInMode & ~(ENABLE_ECHO_INPUT | ENABLE_LINE_INPUT);
        SetConsoleMode(c->hIn, m);
    } else if (c->inMode_saved) {
        SetConsoleMode(c->hIn, c->originalInMode);
    }
#else
    if (enable) {
        if (!c->termios_saved) {
            tcgetattr(STDIN_FILENO, &c->original_termios);
            c->termios_saved = true;
        }
        struct termios raw = c->original_termios;
        raw.c_lflag &= ~(ECHO | ICANON);
        raw.c_cc[VMIN]  = 1;
        raw.c_cc[VTIME] = 0;
        tcsetattr(STDIN_FILENO, TCSANOW, &raw);
    } else if (c->termios_saved) {
        tcsetattr(STDIN_FILENO, TCSANOW, &c->original_termios);
    }
#endif
    c->raw_mode = enable;
    return zym_newNull();
}

// ---- methods (queries) --------------------------------------------------

static ZymValue c_getWidth(ZymVM*, ZymValue ctx) {
    ConsoleData* c = unwrap(ctx); getConsoleSize(c); return zym_newNumber((double)c->width);
}
static ZymValue c_getHeight(ZymVM*, ZymValue ctx) {
    ConsoleData* c = unwrap(ctx); getConsoleSize(c); return zym_newNumber((double)c->height);
}
static ZymValue c_isTTY(ZymVM*, ZymValue ctx) {
    ConsoleData* c = unwrap(ctx);
    return zym_newBool(ZYM_ISATTY(ZYM_FILENO(c->out)) != 0);
}
static ZymValue c_isTTYErr(ZymVM*, ZymValue ctx) {
    ConsoleData* c = unwrap(ctx);
    return zym_newBool(ZYM_ISATTY(ZYM_FILENO(c->err)) != 0);
}

// ---- singleton assembly -------------------------------------------------

ZymValue nativeConsole_create(ZymVM* vm) {
    ConsoleData* c = new ConsoleData();
    c->out = stdout; c->in = stdin; c->err = stderr;
    c->foreground = -1; c->background = -1;
    c->use_rgb_fg = false; c->use_rgb_bg = false;
    c->fg_r = c->fg_g = c->fg_b = 0;
    c->bg_r = c->bg_g = c->bg_b = 0;
    c->bold = c->dim = c->italic = false;
    c->underline = c->reverse = c->strikethrough = false;
    c->cursor_x = c->cursor_y = 0;
    c->cursor_visible = true;
    c->width = 80; c->height = 24;
    c->raw_mode = false; c->alt_screen = false;
#ifdef _WIN32
    c->hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    c->hIn  = GetStdHandle(STD_INPUT_HANDLE);
    GetConsoleMode(c->hOut, &c->originalOutMode);
    c->inMode_saved   = false;
    c->originalInMode = 0;
    c->originalOutCP  = GetConsoleOutputCP();
    DWORD m = c->originalOutMode | ENABLE_VIRTUAL_TERMINAL_PROCESSING;
    SetConsoleMode(c->hOut, m);
    SetConsoleOutputCP(CP_UTF8);
#else
    c->termios_saved = false;
    setlocale(LC_ALL, "");
#endif
    getConsoleSize(c);

    ZymValue ctx = zym_createNativeContext(vm, c, consoleFinalizer);
    zym_pushRoot(vm, ctx);

#define M(NAME, SIG, FN) \
    ZymValue NAME = zym_createNativeClosure(vm, SIG, (void*)FN, ctx); \
    zym_pushRoot(vm, NAME);
#define MV(NAME, SIG, FN) \
    ZymValue NAME = zym_createNativeClosureVariadic(vm, SIG, (void*)FN, ctx); \
    zym_pushRoot(vm, NAME);

    M(write,                 "write(text)",                  c_write)
    M(writeLine,             "writeLine(text)",              c_writeLine)
    M(writeErr,              "writeErr(text)",               c_writeErr)
    M(writeLineErr,          "writeLineErr(text)",           c_writeLineErr)
    M(writeBuffer,           "writeBuffer(buf)",             c_writeBuffer)
    M(writeBufferErr,        "writeBufferErr(buf)",          c_writeBufferErr)
    M(flush,                 "flush()",                      c_flush)

    M(setColor,              "setColor(color)",              c_setColor)
    M(setBackgroundColor,    "setBackgroundColor(color)",    c_setBackgroundColor)
    M(setColorRGB,           "setColorRGB(r,g,b)",           c_setColorRGB)
    M(setBackgroundColorRGB, "setBackgroundColorRGB(r,g,b)", c_setBackgroundColorRGB)
    M(reset,                 "reset()",                      c_reset)

    M(setBold,               "setBold(on)",                  c_setBold)
    M(setDim,                "setDim(on)",                   c_setDim)
    M(setItalic,             "setItalic(on)",                c_setItalic)
    M(setUnderline,          "setUnderline(on)",             c_setUnderline)
    M(setReverse,            "setReverse(on)",               c_setReverse)
    M(setStrikethrough,      "setStrikethrough(on)",         c_setStrikethrough)

    M (moveCursor,           "moveCursor(x,y)",              c_moveCursor)
    MV(moveCursorUp,         "moveCursorUp(...)",            c_moveCursorUp)
    MV(moveCursorDown,       "moveCursorDown(...)",          c_moveCursorDown)
    MV(moveCursorLeft,       "moveCursorLeft(...)",          c_moveCursorLeft)
    MV(moveCursorRight,      "moveCursorRight(...)",         c_moveCursorRight)
    M (hideCursor,           "hideCursor()",                 c_hideCursor)
    M (showCursor,           "showCursor()",                 c_showCursor)
    M (saveCursorPos,        "saveCursorPos()",              c_saveCursorPos)
    M (restoreCursorPos,     "restoreCursorPos()",           c_restoreCursorPos)

    M (clear,                "clear()",                      c_clear)
    M (clearLine,            "clearLine()",                  c_clearLine)
    M (clearToEndOfLine,     "clearToEndOfLine()",           c_clearToEndOfLine)
    M (clearToStartOfLine,   "clearToStartOfLine()",         c_clearToStartOfLine)
    MV(scrollUp,             "scrollUp(...)",                c_scrollUp)
    MV(scrollDown,           "scrollDown(...)",              c_scrollDown)
    M (useAltScreen,         "useAltScreen()",               c_useAltScreen)
    M (useMainScreen,        "useMainScreen()",              c_useMainScreen)
    M (setTitle,             "setTitle(text)",               c_setTitle)
    M (beep,                 "beep()",                       c_beep)

    M (readLine,             "readLine()",                   c_readLine)
    M (readChar,             "readChar()",                   c_readChar)
    M (hasInput,             "hasInput()",                   c_hasInput)
    M (setRawMode,           "setRawMode(on)",               c_setRawMode)

    M (getWidth,             "getWidth()",                   c_getWidth)
    M (getHeight,            "getHeight()",                  c_getHeight)
    M (isTTY,                "isTTY()",                      c_isTTY)
    M (isTTYErr,             "isTTYErr()",                   c_isTTYErr)

#undef M
#undef MV

    ZymValue obj = zym_newMap(vm);
    zym_pushRoot(vm, obj);

    zym_mapSet(vm, obj, "write",                 write);
    zym_mapSet(vm, obj, "writeLine",             writeLine);
    zym_mapSet(vm, obj, "writeErr",              writeErr);
    zym_mapSet(vm, obj, "writeLineErr",          writeLineErr);
    zym_mapSet(vm, obj, "writeBuffer",           writeBuffer);
    zym_mapSet(vm, obj, "writeBufferErr",        writeBufferErr);
    zym_mapSet(vm, obj, "flush",                 flush);

    zym_mapSet(vm, obj, "setColor",              setColor);
    zym_mapSet(vm, obj, "setBackgroundColor",    setBackgroundColor);
    zym_mapSet(vm, obj, "setColorRGB",           setColorRGB);
    zym_mapSet(vm, obj, "setBackgroundColorRGB", setBackgroundColorRGB);
    zym_mapSet(vm, obj, "reset",                 reset);

    zym_mapSet(vm, obj, "setBold",               setBold);
    zym_mapSet(vm, obj, "setDim",                setDim);
    zym_mapSet(vm, obj, "setItalic",             setItalic);
    zym_mapSet(vm, obj, "setUnderline",          setUnderline);
    zym_mapSet(vm, obj, "setReverse",            setReverse);
    zym_mapSet(vm, obj, "setStrikethrough",      setStrikethrough);

    zym_mapSet(vm, obj, "moveCursor",            moveCursor);
    zym_mapSet(vm, obj, "moveCursorUp",          moveCursorUp);
    zym_mapSet(vm, obj, "moveCursorDown",        moveCursorDown);
    zym_mapSet(vm, obj, "moveCursorLeft",        moveCursorLeft);
    zym_mapSet(vm, obj, "moveCursorRight",       moveCursorRight);
    zym_mapSet(vm, obj, "hideCursor",            hideCursor);
    zym_mapSet(vm, obj, "showCursor",            showCursor);
    zym_mapSet(vm, obj, "saveCursorPos",         saveCursorPos);
    zym_mapSet(vm, obj, "restoreCursorPos",      restoreCursorPos);

    zym_mapSet(vm, obj, "clear",                 clear);
    zym_mapSet(vm, obj, "clearLine",             clearLine);
    zym_mapSet(vm, obj, "clearToEndOfLine",      clearToEndOfLine);
    zym_mapSet(vm, obj, "clearToStartOfLine",    clearToStartOfLine);
    zym_mapSet(vm, obj, "scrollUp",              scrollUp);
    zym_mapSet(vm, obj, "scrollDown",            scrollDown);
    zym_mapSet(vm, obj, "useAltScreen",          useAltScreen);
    zym_mapSet(vm, obj, "useMainScreen",         useMainScreen);
    zym_mapSet(vm, obj, "setTitle",              setTitle);
    zym_mapSet(vm, obj, "beep",                  beep);

    zym_mapSet(vm, obj, "readLine",              readLine);
    zym_mapSet(vm, obj, "readChar",              readChar);
    zym_mapSet(vm, obj, "hasInput",              hasInput);
    zym_mapSet(vm, obj, "setRawMode",            setRawMode);

    zym_mapSet(vm, obj, "getWidth",              getWidth);
    zym_mapSet(vm, obj, "getHeight",             getHeight);
    zym_mapSet(vm, obj, "isTTY",                 isTTY);
    zym_mapSet(vm, obj, "isTTYErr",              isTTYErr);

    // ctx + 46 methods + obj = 48
    for (int i = 0; i < 48; i++) zym_popRoot(vm);
    return obj;
}
