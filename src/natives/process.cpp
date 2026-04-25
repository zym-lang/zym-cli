// Process native. Ported from the original C process.c, aligned with the
// instance + statics pattern used by File/Dir/Buffer.
//
// Cross-platform: Unix/Linux uses fork+execvp + (open)pty; Windows uses
// CreateProcess + pipes, with optional ConPTY (Windows 10 1809+).

#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#  include <io.h>
#  include <process.h>
#  include <direct.h>
// ConPTY decls (provide if missing in older MinGW SDKs).
#  ifndef PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE
#    define PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE \
        ProcThreadAttributeValue(22, FALSE, TRUE, FALSE)
typedef VOID* HPCON;
extern "C" HRESULT WINAPI CreatePseudoConsole(COORD size, HANDLE hInput, HANDLE hOutput, DWORD dwFlags, HPCON* phPC);
extern "C" void   WINAPI ClosePseudoConsole(HPCON hPC);
extern "C" HRESULT WINAPI ResizePseudoConsole(HPCON hPC, COORD size);
#  endif
#else
#  include <fcntl.h>
#  include <poll.h>
#  include <signal.h>
#  include <sys/select.h>
#  include <sys/types.h>
#  include <sys/wait.h>
#  include <unistd.h>
#  if defined(__linux__)
#    include <pty.h>
#    include <utmp.h>
#  elif defined(__APPLE__) || defined(__FreeBSD__)
#    include <util.h>
#  endif
extern char** environ;
#endif

#include "core/string/ustring.h"
#include "core/variant/variant.h"

#include "natives.hpp"

// Imported from buffer.cpp.
extern ZymValue makeBufferInstance(ZymVM* vm, const PackedByteArray& src);

namespace {

enum StdioMode { STDIO_PIPE, STDIO_INHERIT, STDIO_NULL, STDIO_PTY };

struct ProcessHandle {
#if defined(_WIN32)
    HANDLE hProcess = INVALID_HANDLE_VALUE;
    HANDLE hThread  = INVALID_HANDLE_VALUE;
    HANDLE hStdin   = INVALID_HANDLE_VALUE;
    HANDLE hStdout  = INVALID_HANDLE_VALUE;
    HANDLE hStderr  = INVALID_HANDLE_VALUE;
    HPCON  hConPTY  = (HPCON)INVALID_HANDLE_VALUE;
    DWORD  processId = 0;
    bool   use_conpty = false;
#else
    pid_t pid = -1;
    int   stdin_fd  = -1;
    int   stdout_fd = -1;
    int   stderr_fd = -1;
    int   pty_master = -1;
    bool  use_pty = false;
#endif
    bool  is_running = false;
    bool  exit_code_valid = false;
    int   exit_code = 0;
    bool  stdin_open = false;
    bool  stdout_open = false;
    bool  stderr_open = false;
};

#if !defined(_WIN32)
void set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0) fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}
void close_fd(int& fd) { if (fd >= 0) { ::close(fd); fd = -1; } }
#else
static inline void close_h(HANDLE& h) {
    if (h && h != INVALID_HANDLE_VALUE) { CloseHandle(h); h = INVALID_HANDLE_VALUE; }
}
#endif

void procFinalizer(ZymVM*, void* data) {
    auto* p = static_cast<ProcessHandle*>(data);
    if (!p) return;
#if defined(_WIN32)
    if (p->is_running && p->hProcess != INVALID_HANDLE_VALUE) {
        TerminateProcess(p->hProcess, 1);
        WaitForSingleObject(p->hProcess, 1000);
    }
    if (p->hConPTY != (HPCON)INVALID_HANDLE_VALUE) ClosePseudoConsole(p->hConPTY);
    close_h(p->hStdin);
    close_h(p->hStdout);
    close_h(p->hStderr);
    close_h(p->hThread);
    close_h(p->hProcess);
#else
    close_fd(p->stdin_fd);
    if (!p->use_pty) { close_fd(p->stdout_fd); close_fd(p->stderr_fd); }
    close_fd(p->pty_master);
    if (p->is_running && p->pid > 0) {
        // best-effort reap so the child does not become a zombie.
        kill(p->pid, SIGKILL);
        int status = 0;
        waitpid(p->pid, &status, 0);
    }
#endif
    delete p;
}

ProcessHandle* unwrapProc(ZymValue ctx) {
    return static_cast<ProcessHandle*>(zym_getNativeData(ctx));
}

bool reqStr(ZymVM* vm, ZymValue v, const char* where, String* out) {
    if (!zym_isString(v)) { zym_runtimeError(vm, "%s expects a string", where); return false; }
    *out = String::utf8(zym_asCString(v)); return true;
}

bool reqProc(ZymVM* vm, ZymValue ctx, const char* where, ProcessHandle** out) {
    auto* p = unwrapProc(ctx);
    if (!p) { zym_runtimeError(vm, "%s: invalid Process handle", where); return false; }
    *out = p; return true;
}

// Resolve a Buffer arg -> PackedByteArray*.
bool reqBufferArg(ZymVM* vm, ZymValue v, const char* where, PackedByteArray** out) {
    if (zym_isMap(v)) {
        ZymValue ctx = zym_mapGet(vm, v, "__pba__");
        if (ctx != ZYM_ERROR) {
            void* data = zym_getNativeData(ctx);
            if (data) { *out = static_cast<PackedByteArray*>(data); return true; }
        }
    }
    zym_runtimeError(vm, "%s expects a Buffer", where);
    return false;
}

StdioMode parseStdio(const char* s, bool* use_pty) {
    if (!strcmp(s, "inherit")) return STDIO_INHERIT;
    if (!strcmp(s, "null"))    return STDIO_NULL;
    if (!strcmp(s, "pty"))     { *use_pty = true; return STDIO_PTY; }
    return STDIO_PIPE;
}

// Read mode strings out of the options map. Defaults: all pipe.
void readStdioModes(ZymVM* vm, ZymValue options,
                    StdioMode* in, StdioMode* out, StdioMode* err, bool* use_pty) {
    *in = STDIO_PIPE; *out = STDIO_PIPE; *err = STDIO_PIPE; *use_pty = false;
    if (zym_isNull(options) || !zym_isMap(options)) return;
    ZymValue v;
    v = zym_mapGet(vm, options, "stdin");
    if (v != ZYM_ERROR && zym_isString(v))  *in  = parseStdio(zym_asCString(v), use_pty);
    v = zym_mapGet(vm, options, "stdout");
    if (v != ZYM_ERROR && zym_isString(v))  *out = parseStdio(zym_asCString(v), use_pty);
    v = zym_mapGet(vm, options, "stderr");
    if (v != ZYM_ERROR && zym_isString(v))  *err = parseStdio(zym_asCString(v), use_pty);
}

#if !defined(_WIN32)
// Spawn helper. Returns true on success and fills `proc`.
bool spawnUnix(ZymVM* vm, ProcessHandle* proc, const String& command,
               ZymValue argsVal, ZymValue options) {
    StdioMode mIn, mOut, mErr; bool use_pty = false;
    readStdioModes(vm, options, &mIn, &mOut, &mErr, &use_pty);

    int inP[2]  = {-1,-1}, outP[2] = {-1,-1}, errP[2] = {-1,-1};
    int pty_m = -1, pty_s = -1;

    auto cleanup_pipes = [&]{
        if (inP[0]  >= 0) { ::close(inP[0]);  ::close(inP[1]);  }
        if (outP[0] >= 0) { ::close(outP[0]); ::close(outP[1]); }
        if (errP[0] >= 0) { ::close(errP[0]); ::close(errP[1]); }
        if (pty_m >= 0)   { ::close(pty_m); ::close(pty_s);     }
    };

    if (use_pty) {
#if defined(__linux__) || defined(__APPLE__) || defined(__FreeBSD__)
        if (openpty(&pty_m, &pty_s, nullptr, nullptr, nullptr) < 0) return false;
        set_nonblocking(pty_m);
#else
        return false;
#endif
    } else {
        if (mIn  == STDIO_PIPE && pipe(inP)  < 0) { cleanup_pipes(); return false; }
        if (mOut == STDIO_PIPE && pipe(outP) < 0) { cleanup_pipes(); return false; }
        if (mErr == STDIO_PIPE && pipe(errP) < 0) { cleanup_pipes(); return false; }
    }

    // Build argv.
    int extra = 0;
    if (!zym_isNull(argsVal) && zym_isList(argsVal)) extra = zym_listLength(argsVal);
    char** argv = (char**)calloc(extra + 2, sizeof(char*));
    if (!argv) { cleanup_pipes(); return false; }
    CharString cmdU = command.utf8();
    argv[0] = strdup(cmdU.get_data());
    for (int i = 0; i < extra; i++) {
        ZymValue a = zym_listGet(vm, argsVal, i);
        if (a != ZYM_ERROR && zym_isString(a)) argv[i + 1] = strdup(zym_asCString(a));
        else                                   argv[i + 1] = strdup("");
    }
    argv[extra + 1] = nullptr;

    // Optional cwd.
    String cwd;
    if (!zym_isNull(options) && zym_isMap(options)) {
        ZymValue cv = zym_mapGet(vm, options, "cwd");
        if (cv != ZYM_ERROR && zym_isString(cv)) cwd = String::utf8(zym_asCString(cv));
    }
    CharString cwdU = cwd.utf8();
    const char* cwdC = cwd.is_empty() ? nullptr : cwdU.get_data();

    pid_t pid = fork();
    if (pid < 0) {
        for (int i = 0; argv[i]; i++) free(argv[i]);
        free(argv);
        cleanup_pipes();
        return false;
    }

    if (pid == 0) {
        // ---- Child ----
        if (use_pty) {
            ::close(pty_m);
            setsid();
#ifdef TIOCSCTTY
            ioctl(pty_s, TIOCSCTTY, 0);
#endif
            dup2(pty_s, STDIN_FILENO);
            dup2(pty_s, STDOUT_FILENO);
            dup2(pty_s, STDERR_FILENO);
            if (pty_s > 2) ::close(pty_s);
        } else {
            if (mIn == STDIO_PIPE) {
                ::close(inP[1]); dup2(inP[0], STDIN_FILENO); ::close(inP[0]);
            } else if (mIn == STDIO_NULL) {
                int n = ::open("/dev/null", O_RDONLY); if (n >= 0) { dup2(n, STDIN_FILENO); ::close(n); }
            }
            if (mOut == STDIO_PIPE) {
                ::close(outP[0]); dup2(outP[1], STDOUT_FILENO); ::close(outP[1]);
            } else if (mOut == STDIO_NULL) {
                int n = ::open("/dev/null", O_WRONLY); if (n >= 0) { dup2(n, STDOUT_FILENO); ::close(n); }
            }
            if (mErr == STDIO_PIPE) {
                ::close(errP[0]); dup2(errP[1], STDERR_FILENO); ::close(errP[1]);
            } else if (mErr == STDIO_NULL) {
                int n = ::open("/dev/null", O_WRONLY); if (n >= 0) { dup2(n, STDERR_FILENO); ::close(n); }
            }
        }
        if (cwdC) { if (chdir(cwdC) != 0) _exit(127); }
        execvp(argv[0], argv);
        _exit(127);
    }

    // ---- Parent ----
    for (int i = 0; argv[i]; i++) free(argv[i]);
    free(argv);

    if (use_pty) {
        ::close(pty_s);
        proc->pty_master = pty_m;
        proc->use_pty = true;
        proc->stdin_fd  = pty_m;
        proc->stdout_fd = pty_m;
        proc->stderr_fd = -1;
        proc->stdin_open = true;
        proc->stdout_open = true;
        proc->stderr_open = false;
    } else {
        if (mIn == STDIO_PIPE)  { ::close(inP[0]);  proc->stdin_fd  = inP[1];  proc->stdin_open  = true; set_nonblocking(proc->stdin_fd); }
        if (mOut == STDIO_PIPE) { ::close(outP[1]); proc->stdout_fd = outP[0]; proc->stdout_open = true; set_nonblocking(proc->stdout_fd); }
        if (mErr == STDIO_PIPE) { ::close(errP[1]); proc->stderr_fd = errP[0]; proc->stderr_open = true; set_nonblocking(proc->stderr_fd); }
    }
    proc->pid = pid;
    proc->is_running = true;
    return true;
}
#endif // !_WIN32

#if defined(_WIN32)
// Build a Windows-style command line by quoting any args containing
// whitespace or quotes, doubling embedded quotes (CommandLineToArgvW rules).
static void appendQuoted(String& out, const String& arg) {
    bool needs = arg.is_empty() || arg.find(" ") >= 0 || arg.find("\t") >= 0 || arg.find("\"") >= 0;
    if (!needs) { out += arg; return; }
    out += "\"";
    for (int i = 0; i < arg.length(); i++) {
        char32_t c = arg[i];
        if (c == '\"') out += "\\\"";
        else { String s; s += String::chr(c); out += s; }
    }
    out += "\"";
}

bool spawnWindows(ZymVM* vm, ProcessHandle* proc, const String& command,
                  ZymValue argsVal, ZymValue options) {
    StdioMode mIn, mOut, mErr; bool use_pty = false;
    readStdioModes(vm, options, &mIn, &mOut, &mErr, &use_pty);

    // Compose command line: command [args...]
    String cmdline; appendQuoted(cmdline, command);
    if (!zym_isNull(argsVal) && zym_isList(argsVal)) {
        int n = zym_listLength(argsVal);
        for (int i = 0; i < n; i++) {
            ZymValue a = zym_listGet(vm, argsVal, i);
            if (a != ZYM_ERROR && zym_isString(a)) {
                cmdline += " ";
                appendQuoted(cmdline, String::utf8(zym_asCString(a)));
            }
        }
    }
    CharString cmdU = cmdline.utf8();
    // CreateProcessA needs a writable buffer.
    size_t cl_len = strlen(cmdU.get_data());
    if (cl_len > 32767) return false;
    char* mutable_cmd = (char*)malloc(cl_len + 1);
    if (!mutable_cmd) return false;
    memcpy(mutable_cmd, cmdU.get_data(), cl_len + 1);

    // Optional cwd.
    String cwd;
    if (!zym_isNull(options) && zym_isMap(options)) {
        ZymValue cv = zym_mapGet(vm, options, "cwd");
        if (cv != ZYM_ERROR && zym_isString(cv)) cwd = String::utf8(zym_asCString(cv));
    }
    CharString cwdU = cwd.utf8();
    const char* cwdC = cwd.is_empty() ? nullptr : cwdU.get_data();

    SECURITY_ATTRIBUTES sa = { sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE };

    // ---- ConPTY mode (any stream marked "pty") ----
    if (use_pty) {
        HANDLE hInR = INVALID_HANDLE_VALUE, hInW = INVALID_HANDLE_VALUE;
        HANDLE hOutR = INVALID_HANDLE_VALUE, hOutW = INVALID_HANDLE_VALUE;
        if (!CreatePipe(&hInR, &hInW, &sa, 0) || !CreatePipe(&hOutR, &hOutW, &sa, 0)) {
            free(mutable_cmd); return false;
        }
        SetHandleInformation(hInW,  HANDLE_FLAG_INHERIT, 0);
        SetHandleInformation(hOutR, HANDLE_FLAG_INHERIT, 0);

        COORD sz = { 80, 25 };
        HPCON hPC = (HPCON)INVALID_HANDLE_VALUE;
        HRESULT hr = CreatePseudoConsole(sz, hInR, hOutW, 0, &hPC);
        // Once owned by ConPTY, child-side handles can be closed.
        CloseHandle(hInR);
        CloseHandle(hOutW);
        if (FAILED(hr)) {
            CloseHandle(hInW); CloseHandle(hOutR);
            free(mutable_cmd); return false;
        }

        SIZE_T attrSz = 0;
        InitializeProcThreadAttributeList(nullptr, 1, 0, &attrSz);
        LPPROC_THREAD_ATTRIBUTE_LIST attrs = (LPPROC_THREAD_ATTRIBUTE_LIST)malloc(attrSz);
        if (!attrs || !InitializeProcThreadAttributeList(attrs, 1, 0, &attrSz) ||
            !UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE,
                                       hPC, sizeof(HPCON), nullptr, nullptr)) {
            if (attrs) { DeleteProcThreadAttributeList(attrs); free(attrs); }
            ClosePseudoConsole(hPC);
            CloseHandle(hInW); CloseHandle(hOutR);
            free(mutable_cmd); return false;
        }

        STARTUPINFOEXA siEx; ZeroMemory(&siEx, sizeof(siEx));
        siEx.StartupInfo.cb = sizeof(STARTUPINFOEXA);
        siEx.lpAttributeList = attrs;
        PROCESS_INFORMATION pi; ZeroMemory(&pi, sizeof(pi));
        BOOL ok = CreateProcessA(nullptr, mutable_cmd, nullptr, nullptr, FALSE,
                                 EXTENDED_STARTUPINFO_PRESENT, nullptr, cwdC,
                                 &siEx.StartupInfo, &pi);
        DeleteProcThreadAttributeList(attrs);
        free(attrs);
        free(mutable_cmd);
        if (!ok) {
            ClosePseudoConsole(hPC);
            CloseHandle(hInW); CloseHandle(hOutR);
            return false;
        }
        proc->hProcess  = pi.hProcess;
        proc->hThread   = pi.hThread;
        proc->processId = pi.dwProcessId;
        proc->hConPTY   = hPC;
        proc->use_conpty = true;
        proc->hStdin    = hInW;
        proc->hStdout   = hOutR;
        proc->hStderr   = INVALID_HANDLE_VALUE;
        proc->stdin_open  = true;
        proc->stdout_open = true;
        proc->stderr_open = false;
        proc->is_running = true;
        return true;
    }

    // ---- Plain pipe / inherit / null mode ----
    HANDLE hInR = INVALID_HANDLE_VALUE, hInW = INVALID_HANDLE_VALUE;
    HANDLE hOutR = INVALID_HANDLE_VALUE, hOutW = INVALID_HANDLE_VALUE;
    HANDLE hErrR = INVALID_HANDLE_VALUE, hErrW = INVALID_HANDLE_VALUE;

    if (mIn == STDIO_PIPE) {
        if (!CreatePipe(&hInR, &hInW, &sa, 0)) { free(mutable_cmd); return false; }
        SetHandleInformation(hInW, HANDLE_FLAG_INHERIT, 0);
        proc->hStdin = hInW; proc->stdin_open = true;
    } else if (mIn == STDIO_NULL) {
        hInR = CreateFileA("NUL", GENERIC_READ, 0, &sa, OPEN_EXISTING, 0, nullptr);
    }
    if (mOut == STDIO_PIPE) {
        if (!CreatePipe(&hOutR, &hOutW, &sa, 0)) { free(mutable_cmd); return false; }
        SetHandleInformation(hOutR, HANDLE_FLAG_INHERIT, 0);
        proc->hStdout = hOutR; proc->stdout_open = true;
    } else if (mOut == STDIO_NULL) {
        hOutW = CreateFileA("NUL", GENERIC_WRITE, 0, &sa, OPEN_EXISTING, 0, nullptr);
    }
    if (mErr == STDIO_PIPE) {
        if (!CreatePipe(&hErrR, &hErrW, &sa, 0)) { free(mutable_cmd); return false; }
        SetHandleInformation(hErrR, HANDLE_FLAG_INHERIT, 0);
        proc->hStderr = hErrR; proc->stderr_open = true;
    } else if (mErr == STDIO_NULL) {
        hErrW = CreateFileA("NUL", GENERIC_WRITE, 0, &sa, OPEN_EXISTING, 0, nullptr);
    }

    STARTUPINFOA si; ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput  = (mIn  == STDIO_INHERIT) ? GetStdHandle(STD_INPUT_HANDLE)  : hInR;
    si.hStdOutput = (mOut == STDIO_INHERIT) ? GetStdHandle(STD_OUTPUT_HANDLE) : hOutW;
    si.hStdError  = (mErr == STDIO_INHERIT) ? GetStdHandle(STD_ERROR_HANDLE)  : hErrW;

    PROCESS_INFORMATION pi; ZeroMemory(&pi, sizeof(pi));
    BOOL ok = CreateProcessA(nullptr, mutable_cmd, nullptr, nullptr, TRUE, 0,
                             nullptr, cwdC, &si, &pi);
    free(mutable_cmd);

    // Always close the child-end handles we own.
    if (hInR  != INVALID_HANDLE_VALUE && mIn  != STDIO_INHERIT) CloseHandle(hInR);
    if (hOutW != INVALID_HANDLE_VALUE && mOut != STDIO_INHERIT) CloseHandle(hOutW);
    if (hErrW != INVALID_HANDLE_VALUE && mErr != STDIO_INHERIT) CloseHandle(hErrW);

    if (!ok) {
        close_h(proc->hStdin);
        close_h(proc->hStdout);
        close_h(proc->hStderr);
        return false;
    }
    proc->hProcess  = pi.hProcess;
    proc->hThread   = pi.hThread;
    proc->processId = pi.dwProcessId;
    proc->is_running = true;
    return true;
}
#endif // _WIN32

// Cross-platform spawn dispatch.
static inline bool spawnPlatform(ZymVM* vm, ProcessHandle* p, const String& cmd,
                                 ZymValue argsV, ZymValue optsV) {
#if defined(_WIN32)
    return spawnWindows(vm, p, cmd, argsV, optsV);
#else
    return spawnUnix(vm, p, cmd, argsV, optsV);
#endif
}

// ---- instance methods ----

ZymValue i_write(ZymVM* vm, ZymValue ctx, ZymValue dataV) {
    ProcessHandle* p; if (!reqProc(vm, ctx, "Process.write(s)", &p)) return ZYM_ERROR;
    if (!p->stdin_open) { zym_runtimeError(vm, "Process.write(s): stdin is not open"); return ZYM_ERROR; }
    if (!zym_isString(dataV)) { zym_runtimeError(vm, "Process.write(s) expects a string"); return ZYM_ERROR; }
    const char* s = zym_asCString(dataV);
    size_t len = strlen(s);
#if defined(_WIN32)
    DWORD written = 0;
    if (!WriteFile(p->hStdin, s, (DWORD)len, &written, nullptr)) {
        zym_runtimeError(vm, "Process.write(s): WriteFile failed (%lu)", (unsigned long)GetLastError());
        return ZYM_ERROR;
    }
    return zym_newNumber((double)written);
#else
    ssize_t n = ::write(p->stdin_fd, s, len);
    if (n < 0) { zym_runtimeError(vm, "Process.write(s): %s", strerror(errno)); return ZYM_ERROR; }
    return zym_newNumber((double)n);
#endif
}

ZymValue i_writeBuffer(ZymVM* vm, ZymValue ctx, ZymValue bufV) {
    ProcessHandle* p; if (!reqProc(vm, ctx, "Process.writeBuffer(buf)", &p)) return ZYM_ERROR;
    if (!p->stdin_open) { zym_runtimeError(vm, "Process.writeBuffer(buf): stdin is not open"); return ZYM_ERROR; }
    PackedByteArray* buf; if (!reqBufferArg(vm, bufV, "Process.writeBuffer(buf)", &buf)) return ZYM_ERROR;
    int sz = buf->size();
    if (sz == 0) return zym_newNumber(0);
#if defined(_WIN32)
    DWORD written = 0;
    if (!WriteFile(p->hStdin, buf->ptr(), (DWORD)sz, &written, nullptr)) {
        zym_runtimeError(vm, "Process.writeBuffer(buf): WriteFile failed (%lu)", (unsigned long)GetLastError());
        return ZYM_ERROR;
    }
    return zym_newNumber((double)written);
#else
    ssize_t n = ::write(p->stdin_fd, buf->ptr(), sz);
    if (n < 0) { zym_runtimeError(vm, "Process.writeBuffer(buf): %s", strerror(errno)); return ZYM_ERROR; }
    return zym_newNumber((double)n);
#endif
}

ZymValue i_closeStdin(ZymVM*, ZymValue ctx) {
    auto* p = unwrapProc(ctx);
    if (p && p->stdin_open) {
#if defined(_WIN32)
        close_h(p->hStdin);
#else
        if (!p->use_pty) close_fd(p->stdin_fd);
#endif
        p->stdin_open = false;
    }
    return zym_newNull();
}

// Read one chunk (up to 4096 bytes); returns Buffer (possibly empty).
#if defined(_WIN32)
static ZymValue readChunkWin(ZymVM* vm, HANDLE h, bool* open_flag, const char* where) {
    PackedByteArray pba;
    if (!*open_flag || h == INVALID_HANDLE_VALUE) return makeBufferInstance(vm, pba);
    DWORD avail = 0;
    if (!PeekNamedPipe(h, nullptr, 0, nullptr, &avail, nullptr)) {
        // Pipe broken / closed — treat as EOF.
        *open_flag = false;
        return makeBufferInstance(vm, pba);
    }
    if (avail == 0) return makeBufferInstance(vm, pba);
    uint8_t tmp[4096];
    DWORD toRead = avail < (DWORD)sizeof(tmp) ? avail : (DWORD)sizeof(tmp);
    DWORD got = 0;
    if (!ReadFile(h, tmp, toRead, &got, nullptr)) {
        zym_runtimeError(vm, "%s: ReadFile failed (%lu)", where, (unsigned long)GetLastError());
        return ZYM_ERROR;
    }
    pba.resize((int)got);
    if (got > 0) memcpy(pba.ptrw(), tmp, (size_t)got);
    return makeBufferInstance(vm, pba);
}
static ZymValue drainAvailWin(ZymVM* vm, HANDLE h, bool* open_flag, const char* where) {
    PackedByteArray pba;
    if (!*open_flag || h == INVALID_HANDLE_VALUE) return makeBufferInstance(vm, pba);
    while (true) {
        DWORD avail = 0;
        if (!PeekNamedPipe(h, nullptr, 0, nullptr, &avail, nullptr)) {
            *open_flag = false; break;
        }
        if (avail == 0) break;
        uint8_t tmp[4096];
        DWORD toRead = avail < (DWORD)sizeof(tmp) ? avail : (DWORD)sizeof(tmp);
        DWORD got = 0;
        if (!ReadFile(h, tmp, toRead, &got, nullptr)) {
            zym_runtimeError(vm, "%s: ReadFile failed (%lu)", where, (unsigned long)GetLastError());
            return ZYM_ERROR;
        }
        if (got == 0) break;
        int prev = pba.size();
        pba.resize(prev + (int)got);
        memcpy(pba.ptrw() + prev, tmp, (size_t)got);
    }
    return makeBufferInstance(vm, pba);
}
#else
static ZymValue readChunkUnix(ZymVM* vm, int fd, bool* open_flag, const char* where) {
    if (!*open_flag || fd < 0) return makeBufferInstance(vm, PackedByteArray());
    uint8_t tmp[4096];
    ssize_t n = ::read(fd, tmp, sizeof(tmp));
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return makeBufferInstance(vm, PackedByteArray());
        zym_runtimeError(vm, "%s: %s", where, strerror(errno));
        return ZYM_ERROR;
    }
    PackedByteArray pba;
    pba.resize((int)n);
    if (n > 0) memcpy(pba.ptrw(), tmp, (size_t)n);
    return makeBufferInstance(vm, pba);
}
static ZymValue drainAvailUnix(ZymVM* vm, int fd, bool* open_flag, const char* where) {
    PackedByteArray pba;
    if (!*open_flag || fd < 0) return makeBufferInstance(vm, pba);
    uint8_t tmp[4096];
    while (true) {
        ssize_t n = ::read(fd, tmp, sizeof(tmp));
        if (n > 0) {
            int prev = pba.size();
            pba.resize(prev + (int)n);
            memcpy(pba.ptrw() + prev, tmp, (size_t)n);
            continue;
        }
        if (n == 0) break;
        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
        zym_runtimeError(vm, "%s: %s", where, strerror(errno));
        return ZYM_ERROR;
    }
    return makeBufferInstance(vm, pba);
}
#endif

ZymValue i_read(ZymVM* vm, ZymValue ctx) {
    ProcessHandle* p; if (!reqProc(vm, ctx, "Process.read()", &p)) return ZYM_ERROR;
#if defined(_WIN32)
    return readChunkWin(vm, p->hStdout, &p->stdout_open, "Process.read()");
#else
    return readChunkUnix(vm, p->stdout_fd, &p->stdout_open, "Process.read()");
#endif
}

ZymValue i_readErr(ZymVM* vm, ZymValue ctx) {
    ProcessHandle* p; if (!reqProc(vm, ctx, "Process.readErr()", &p)) return ZYM_ERROR;
#if defined(_WIN32)
    return readChunkWin(vm, p->hStderr, &p->stderr_open, "Process.readErr()");
#else
    return readChunkUnix(vm, p->stderr_fd, &p->stderr_open, "Process.readErr()");
#endif
}

ZymValue i_readNonBlock(ZymVM* vm, ZymValue ctx) {
    ProcessHandle* p; if (!reqProc(vm, ctx, "Process.readNonBlock()", &p)) return ZYM_ERROR;
#if defined(_WIN32)
    return drainAvailWin(vm, p->hStdout, &p->stdout_open, "Process.readNonBlock()");
#else
    return drainAvailUnix(vm, p->stdout_fd, &p->stdout_open, "Process.readNonBlock()");
#endif
}

ZymValue i_readErrNonBlock(ZymVM* vm, ZymValue ctx) {
    ProcessHandle* p; if (!reqProc(vm, ctx, "Process.readErrNonBlock()", &p)) return ZYM_ERROR;
#if defined(_WIN32)
    return drainAvailWin(vm, p->hStderr, &p->stderr_open, "Process.readErrNonBlock()");
#else
    return drainAvailUnix(vm, p->stderr_fd, &p->stderr_open, "Process.readErrNonBlock()");
#endif
}

#if !defined(_WIN32)
int signalFromValue(ZymVM* vm, ZymValue v, bool* ok) {
    *ok = true;
    if (zym_isNull(v))   return SIGTERM;
    if (zym_isNumber(v)) return (int)zym_asNumber(v);
    if (zym_isString(v)) {
        const char* s = zym_asCString(v);
        if (!strcmp(s, "SIGTERM")) return SIGTERM;
        if (!strcmp(s, "SIGKILL")) return SIGKILL;
        if (!strcmp(s, "SIGINT"))  return SIGINT;
        if (!strcmp(s, "SIGHUP"))  return SIGHUP;
        if (!strcmp(s, "SIGQUIT")) return SIGQUIT;
        if (!strcmp(s, "SIGUSR1")) return SIGUSR1;
        if (!strcmp(s, "SIGUSR2")) return SIGUSR2;
        if (!strcmp(s, "SIGSTOP")) return SIGSTOP;
        if (!strcmp(s, "SIGCONT")) return SIGCONT;
        if (!strcmp(s, "SIGPIPE")) return SIGPIPE;
        zym_runtimeError(vm, "Process.kill(signal): unknown signal name %s", s);
        *ok = false; return 0;
    }
    zym_runtimeError(vm, "Process.kill(signal): expected number or string");
    *ok = false; return 0;
}

void harvestStatus(ProcessHandle* p, int status) {
    if (WIFEXITED(status))        p->exit_code = WEXITSTATUS(status);
    else if (WIFSIGNALED(status)) p->exit_code = 128 + WTERMSIG(status);
    else                          p->exit_code = -1;
    p->is_running = false;
    p->exit_code_valid = true;
}
#endif

ZymValue i_kill(ZymVM* vm, ZymValue ctx, ZymValue* vargs, int vargc) {
    ZymValue sigV = vargc >= 1 ? vargs[0] : zym_newNull();
    ProcessHandle* p; if (!reqProc(vm, ctx, "Process.kill(signal)", &p)) return ZYM_ERROR;
    if (!p->is_running) return zym_newBool(false);
#if defined(_WIN32)
    (void)sigV; // Windows has no per-signal selection — always terminate.
    if (!TerminateProcess(p->hProcess, 1)) {
        zym_runtimeError(vm, "Process.kill: TerminateProcess failed (%lu)", (unsigned long)GetLastError());
        return ZYM_ERROR;
    }
    return zym_newBool(true);
#else
    bool ok; int sig = signalFromValue(vm, sigV, &ok);
    if (!ok) return ZYM_ERROR;
    if (kill(p->pid, sig) < 0) { zym_runtimeError(vm, "Process.kill: %s", strerror(errno)); return ZYM_ERROR; }
    return zym_newBool(true);
#endif
}

ZymValue i_wait(ZymVM* vm, ZymValue ctx) {
    ProcessHandle* p; if (!reqProc(vm, ctx, "Process.wait()", &p)) return ZYM_ERROR;
    if (!p->is_running) return zym_newNumber((double)p->exit_code);
#if defined(_WIN32)
    WaitForSingleObject(p->hProcess, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(p->hProcess, &code);
    p->exit_code = (int)code;
    p->is_running = false;
    p->exit_code_valid = true;
    return zym_newNumber((double)p->exit_code);
#else
    int status = 0;
    if (waitpid(p->pid, &status, 0) < 0) {
        zym_runtimeError(vm, "Process.wait(): %s", strerror(errno));
        return ZYM_ERROR;
    }
    harvestStatus(p, status);
    return zym_newNumber((double)p->exit_code);
#endif
}

ZymValue i_poll(ZymVM* vm, ZymValue ctx) {
    ProcessHandle* p; if (!reqProc(vm, ctx, "Process.poll()", &p)) return ZYM_ERROR;
    if (!p->is_running) {
        if (p->exit_code_valid) return zym_newNumber((double)p->exit_code);
        return zym_newNull();
    }
#if defined(_WIN32)
    DWORD r = WaitForSingleObject(p->hProcess, 0);
    if (r == WAIT_OBJECT_0) {
        DWORD code = 0;
        GetExitCodeProcess(p->hProcess, &code);
        p->exit_code = (int)code;
        p->is_running = false;
        p->exit_code_valid = true;
        return zym_newNumber((double)p->exit_code);
    }
    return zym_newNull();
#else
    int status = 0;
    pid_t r = waitpid(p->pid, &status, WNOHANG);
    if (r > 0) { harvestStatus(p, status); return zym_newNumber((double)p->exit_code); }
    return zym_newNull();
#endif
}

ZymValue i_isRunning(ZymVM*, ZymValue ctx) {
    auto* p = unwrapProc(ctx); return zym_newBool(p && p->is_running);
}

ZymValue i_getPid(ZymVM*, ZymValue ctx) {
    auto* p = unwrapProc(ctx);
    if (!p) return zym_newNull();
#if defined(_WIN32)
    return zym_newNumber((double)p->processId);
#else
    return zym_newNumber((double)p->pid);
#endif
}

ZymValue i_getExitCode(ZymVM*, ZymValue ctx) {
    auto* p = unwrapProc(ctx);
    if (!p || !p->exit_code_valid) return zym_newNull();
    return zym_newNumber((double)p->exit_code);
}

// ---- instance assembly ----

ZymValue makeProcessInstance(ZymVM* vm, ProcessHandle* p) {
    ZymValue ctx = zym_createNativeContext(vm, p, procFinalizer);
    zym_pushRoot(vm, ctx);

    ZymValue obj = zym_newMap(vm);
    zym_pushRoot(vm, obj);
    zym_mapSet(vm, obj, "__proc__", ctx);

#define MV(name, sig, fn) do { \
        ZymValue cl = zym_createNativeClosureVariadic(vm, sig, (void*)fn, ctx); \
        zym_pushRoot(vm, cl); zym_mapSet(vm, obj, name, cl); zym_popRoot(vm); \
    } while (0)
#define M(name, sig, fn) do { \
        ZymValue cl = zym_createNativeClosure(vm, sig, (void*)fn, ctx); \
        zym_pushRoot(vm, cl); zym_mapSet(vm, obj, name, cl); zym_popRoot(vm); \
    } while (0)

    M("write",            "write(s)",          i_write);
    M("writeBuffer",      "writeBuffer(buf)",  i_writeBuffer);
    M("closeStdin",       "closeStdin()",      i_closeStdin);
    M("read",             "read()",            i_read);
    M("readErr",          "readErr()",         i_readErr);
    M("readNonBlock",     "readNonBlock()",    i_readNonBlock);
    M("readErrNonBlock",  "readErrNonBlock()", i_readErrNonBlock);
    MV("kill",            "kill(...)",         i_kill);
    M("wait",             "wait()",            i_wait);
    M("poll",             "poll()",            i_poll);
    M("isRunning",        "isRunning()",       i_isRunning);
    M("getPid",           "getPid()",          i_getPid);
    M("getExitCode",      "getExitCode()",     i_getExitCode);

#undef M
#undef MV

    zym_popRoot(vm); // obj
    zym_popRoot(vm); // ctx
    return obj;
}

// ---- statics ----

ZymValue p_spawn(ZymVM* vm, ZymValue, ZymValue commandV, ZymValue* vargs, int vargc) {
    String command;
    if (!reqStr(vm, commandV, "Process.spawn(command, args?, options?)", &command)) return ZYM_ERROR;
    ZymValue argsV = vargc >= 1 ? vargs[0] : zym_newNull();
    ZymValue optsV = vargc >= 2 ? vargs[1] : zym_newNull();
    auto* p = new ProcessHandle();
    if (!spawnPlatform(vm, p, command, argsV, optsV)) {
        delete p;
        return zym_newNull();
    }
    return makeProcessInstance(vm, p);
}

// exec: spawn, close stdin, drain stdout/stderr until exit, return
// { stdout: Buffer, stderr: Buffer, exitCode: number }.
ZymValue p_exec(ZymVM* vm, ZymValue, ZymValue commandV, ZymValue* vargs, int vargc) {
    String command;
    if (!reqStr(vm, commandV, "Process.exec(command, args?, options?)", &command)) return ZYM_ERROR;
    ZymValue argsV = vargc >= 1 ? vargs[0] : zym_newNull();
    ZymValue optsV = vargc >= 2 ? vargs[1] : zym_newNull();

    auto* p = new ProcessHandle();
    if (!spawnPlatform(vm, p, command, argsV, optsV)) {
        delete p;
        zym_runtimeError(vm, "Process.exec: failed to spawn");
        return ZYM_ERROR;
    }

    // Close child's stdin so it can finish if it reads from stdin.
#if defined(_WIN32)
    if (p->stdin_open) { close_h(p->hStdin); p->stdin_open = false; }
#else
    if (p->stdin_open && !p->use_pty) close_fd(p->stdin_fd);
    p->stdin_open = false;
#endif

    PackedByteArray outBuf, errBuf;

#if defined(_WIN32)
    auto drainOnce = [&](HANDLE h, bool& open_flag, PackedByteArray& dst) -> bool {
        if (!open_flag || h == INVALID_HANDLE_VALUE) return false;
        bool got = false;
        while (true) {
            DWORD avail = 0;
            if (!PeekNamedPipe(h, nullptr, 0, nullptr, &avail, nullptr)) { open_flag = false; break; }
            if (avail == 0) break;
            uint8_t tmp[4096];
            DWORD toRead = avail < (DWORD)sizeof(tmp) ? avail : (DWORD)sizeof(tmp);
            DWORD bytes = 0;
            if (!ReadFile(h, tmp, toRead, &bytes, nullptr) || bytes == 0) { open_flag = false; break; }
            int prev = dst.size(); dst.resize(prev + (int)bytes);
            memcpy(dst.ptrw() + prev, tmp, (size_t)bytes);
            got = true;
        }
        return got;
    };
    bool exited = false;
    while (true) {
        if (!exited) {
            DWORD r = WaitForSingleObject(p->hProcess, 0);
            if (r == WAIT_OBJECT_0) {
                DWORD code = 0;
                GetExitCodeProcess(p->hProcess, &code);
                p->exit_code = (int)code;
                p->is_running = false;
                p->exit_code_valid = true;
                exited = true;
            }
        }
        bool a = drainOnce(p->hStdout, p->stdout_open, outBuf);
        bool b = drainOnce(p->hStderr, p->stderr_open, errBuf);
        if (exited && !a && !b) break;
        if (!exited && !a && !b) Sleep(5);
    }
#else
    auto drainOnce = [&](int fd, PackedByteArray& dst) -> bool {
        if (fd < 0) return false;
        uint8_t tmp[4096];
        bool got = false;
        while (true) {
            ssize_t n = ::read(fd, tmp, sizeof(tmp));
            if (n > 0) {
                int prev = dst.size(); dst.resize(prev + (int)n);
                memcpy(dst.ptrw() + prev, tmp, (size_t)n);
                got = true; continue;
            }
            break;
        }
        return got;
    };

    bool exited = false;
    while (true) {
        // poll
        if (!exited) {
            int status = 0;
            pid_t r = waitpid(p->pid, &status, WNOHANG);
            if (r > 0) { harvestStatus(p, status); exited = true; }
        }
        bool a = drainOnce(p->stdout_fd, outBuf);
        bool b = drainOnce(p->stderr_fd, errBuf);
        if (exited && !a && !b) break;
        if (!exited && !a && !b) {
            // sleep ~5ms via poll-with-timeout to avoid busy-spin.
            struct pollfd pfd[2]; int nfd = 0;
            if (p->stdout_fd >= 0) { pfd[nfd].fd = p->stdout_fd; pfd[nfd].events = POLLIN; nfd++; }
            if (p->stderr_fd >= 0) { pfd[nfd].fd = p->stderr_fd; pfd[nfd].events = POLLIN; nfd++; }
            if (nfd == 0) usleep(5000); else poll(pfd, nfd, 5);
        }
    }
#endif

    ZymValue result = zym_newMap(vm);
    zym_pushRoot(vm, result);
    ZymValue outV = makeBufferInstance(vm, outBuf); zym_pushRoot(vm, outV);
    ZymValue errV = makeBufferInstance(vm, errBuf); zym_pushRoot(vm, errV);
    zym_mapSet(vm, result, "stdout",   outV);
    zym_mapSet(vm, result, "stderr",   errV);
    zym_mapSet(vm, result, "exitCode", zym_newNumber((double)p->exit_code));
    zym_popRoot(vm); zym_popRoot(vm);

    // Tear the child handle down now; result no longer needs it.
    procFinalizer(vm, p);

    zym_popRoot(vm);
    return result;
}

ZymValue p_getCwd(ZymVM* vm, ZymValue) {
    char buf[4096];
#if defined(_WIN32)
    if (!_getcwd(buf, sizeof(buf))) { zym_runtimeError(vm, "Process.getCwd: %s", strerror(errno)); return ZYM_ERROR; }
#else
    if (!getcwd(buf, sizeof(buf))) { zym_runtimeError(vm, "Process.getCwd: %s", strerror(errno)); return ZYM_ERROR; }
#endif
    return zym_newStringN(vm, buf, (int)strlen(buf));
}

ZymValue p_setCwd(ZymVM* vm, ZymValue, ZymValue pathV) {
    String path; if (!reqStr(vm, pathV, "Process.setCwd(path)", &path)) return ZYM_ERROR;
    CharString u = path.utf8();
#if defined(_WIN32)
    if (_chdir(u.get_data()) != 0) { zym_runtimeError(vm, "Process.setCwd: %s", strerror(errno)); return ZYM_ERROR; }
#else
    if (chdir(u.get_data()) != 0) { zym_runtimeError(vm, "Process.setCwd: %s", strerror(errno)); return ZYM_ERROR; }
#endif
    return zym_newBool(true);
}

ZymValue p_getEnv(ZymVM* vm, ZymValue, ZymValue keyV) {
    String key; if (!reqStr(vm, keyV, "Process.getEnv(key)", &key)) return ZYM_ERROR;
    CharString u = key.utf8();
    const char* v = getenv(u.get_data());
    if (!v) return zym_newNull();
    return zym_newStringN(vm, v, (int)strlen(v));
}

ZymValue p_setEnv(ZymVM* vm, ZymValue, ZymValue keyV, ZymValue valV) {
    String key; if (!reqStr(vm, keyV, "Process.setEnv(key, value)", &key)) return ZYM_ERROR;
    String val; if (!reqStr(vm, valV, "Process.setEnv(key, value)", &val)) return ZYM_ERROR;
    CharString uk = key.utf8(), uv = val.utf8();
#if defined(_WIN32)
    if (!SetEnvironmentVariableA(uk.get_data(), uv.get_data())) {
        zym_runtimeError(vm, "Process.setEnv: SetEnvironmentVariable failed (%lu)", (unsigned long)GetLastError());
        return ZYM_ERROR;
    }
#else
    if (setenv(uk.get_data(), uv.get_data(), 1) != 0) {
        zym_runtimeError(vm, "Process.setEnv: %s", strerror(errno));
        return ZYM_ERROR;
    }
#endif
    return zym_newBool(true);
}

ZymValue p_unsetEnv(ZymVM* vm, ZymValue, ZymValue keyV) {
    String key; if (!reqStr(vm, keyV, "Process.unsetEnv(key)", &key)) return ZYM_ERROR;
    CharString u = key.utf8();
#if defined(_WIN32)
    return zym_newBool(SetEnvironmentVariableA(u.get_data(), nullptr) != 0);
#else
    return zym_newBool(unsetenv(u.get_data()) == 0);
#endif
}

ZymValue p_getEnvAll(ZymVM* vm, ZymValue) {
    ZymValue m = zym_newMap(vm);
    zym_pushRoot(vm, m);
#if defined(_WIN32)
    LPCH es = GetEnvironmentStringsA();
    if (es) {
        for (LPCH cur = es; *cur; cur += strlen(cur) + 1) {
            const char* eq = strchr(cur, '=');
            if (!eq || eq == cur) continue;
            size_t klen = (size_t)(eq - cur);
            char keyBuf[1024];
            if (klen >= sizeof(keyBuf)) klen = sizeof(keyBuf) - 1;
            memcpy(keyBuf, cur, klen); keyBuf[klen] = 0;
            const char* val = eq + 1;
            ZymValue vs = zym_newStringN(vm, val, (int)strlen(val));
            zym_pushRoot(vm, vs);
            zym_mapSet(vm, m, keyBuf, vs);
            zym_popRoot(vm);
        }
        FreeEnvironmentStringsA(es);
    }
#else
    for (char** e = environ; e && *e; e++) {
        const char* eq = strchr(*e, '=');
        if (!eq || eq == *e) continue;
        size_t klen = (size_t)(eq - *e);
        char keyBuf[1024];
        if (klen >= sizeof(keyBuf)) klen = sizeof(keyBuf) - 1;
        memcpy(keyBuf, *e, klen); keyBuf[klen] = 0;
        const char* val = eq + 1;
        ZymValue vs = zym_newStringN(vm, val, (int)strlen(val));
        zym_pushRoot(vm, vs);
        zym_mapSet(vm, m, keyBuf, vs);
        zym_popRoot(vm);
    }
#endif
    zym_popRoot(vm);
    return m;
}

#if defined(_WIN32)
ZymValue p_getPid(ZymVM*, ZymValue)        { return zym_newNumber((double)GetCurrentProcessId()); }
ZymValue p_getParentPid(ZymVM*, ZymValue)  { return zym_newNull(); /* no portable equivalent on Windows */ }
#else
ZymValue p_getPid(ZymVM*, ZymValue)        { return zym_newNumber((double)getpid()); }
ZymValue p_getParentPid(ZymVM*, ZymValue)  { return zym_newNumber((double)getppid()); }
#endif

ZymValue p_exit(ZymVM* vm, ZymValue, ZymValue* vargs, int vargc) {
    ZymValue codeV = vargc >= 1 ? vargs[0] : zym_newNull();
    int code = 0;
    if (!zym_isNull(codeV)) {
        if (!zym_isNumber(codeV)) { zym_runtimeError(vm, "Process.exit(code): code must be a number"); return ZYM_ERROR; }
        code = (int)zym_asNumber(codeV);
    }
    exit(code);
    return zym_newNull();
}

} // namespace

// ---- factory ----

ZymValue nativeProcess_create(ZymVM* vm) {
    ZymValue ctx = zym_createNativeContext(vm, nullptr, nullptr);
    zym_pushRoot(vm, ctx);

    ZymValue obj = zym_newMap(vm);
    zym_pushRoot(vm, obj);

#define F(name, sig, fn) do { \
        ZymValue cl = zym_createNativeClosure(vm, sig, (void*)fn, ctx); \
        zym_pushRoot(vm, cl); zym_mapSet(vm, obj, name, cl); zym_popRoot(vm); \
    } while (0)
#define FV(name, sig, fn) do { \
        ZymValue cl = zym_createNativeClosureVariadic(vm, sig, (void*)fn, ctx); \
        zym_pushRoot(vm, cl); zym_mapSet(vm, obj, name, cl); zym_popRoot(vm); \
    } while (0)

    FV("spawn",       "spawn(command, ...)",             p_spawn);
    FV("exec",        "exec(command, ...)",              p_exec);
    F("getCwd",       "getCwd()",                        p_getCwd);
    F("setCwd",       "setCwd(path)",                    p_setCwd);
    F("getEnv",       "getEnv(key)",                     p_getEnv);
    F("setEnv",       "setEnv(key, value)",              p_setEnv);
    F("unsetEnv",     "unsetEnv(key)",                   p_unsetEnv);
    F("getEnvAll",    "getEnvAll()",                     p_getEnvAll);
    F("getPid",       "getPid()",                        p_getPid);
    F("getParentPid", "getParentPid()",                  p_getParentPid);
    FV("exit",        "exit(...)",                       p_exit);

#undef F
#undef FV

    zym_popRoot(vm); // obj
    zym_popRoot(vm); // ctx
    return obj;
}
