/* ===========================================================================
 * host_proc.c - uc_proc.h on a desktop OS (UCD-14).
 *
 * Two platforms, one shape:
 *
 *   POSIX    forkpty(), or openpty()+fork() where forkpty is not declared.
 *            The child gets a real terminal, so `git status` colours itself
 *            and `make` line-buffers instead of block-buffering.
 *   Windows  ConPTY (CreatePseudoConsole), resolved at RUN TIME from
 *            kernel32.  It has to be: the mingw headers this cross-builds
 *            against do not declare it, and a build that hard-linked it would
 *            not start at all on a Windows without it.  Where it is missing,
 *            the child gets anonymous PIPES - which is worse (no colour, block
 *            buffering) but is a working terminal rather than an absent one.
 *
 * NON-BLOCKING IS THE WHOLE CONTRACT.  Reads are polled: O_NONBLOCK on POSIX,
 * PeekNamedPipe on Windows.  Nothing here may wait, because the caller is a
 * frame loop that has a workbench to paint while `make` runs.
 * ======================================================================== */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "uc_proc.h"
#include "host.h"

static char g_err[256];
const char *uc_proc_error(void) { return g_err[0] ? g_err : "no error"; }

/* (volume, directory) -> a real directory to start a child in.  The core
 * addresses everything by volume; a shell needs a path. */
int uc_proc_workdir(int vol, const char *dir, char *out, int cap)
{
    const char *root = host_fs_volume_root(vol);
    int n = 0, i;
    if (!out || cap <= 0) return 0;
    out[0] = 0;
    if (!root || !root[0]) return 0;
    while (root[n] && n < cap - 1) { out[n] = root[n]; n++; }
    if (dir && dir[0]) {
        if (n < cap - 1) out[n++] = '/';
        /* the core spells directories with '\\'; the OS does not */
        for (i = 0; dir[i] && n < cap - 1; i++)
            out[n++] = (dir[i] == '\\') ? '/' : dir[i];
    }
    out[n] = 0;
    return 1;
}

/* ======================================================================== */
#ifdef _WIN32
/* ======================================================================== */
#include <windows.h>

/* ConPTY's types, mirrored rather than included: the mingw-w64 headers this
 * builds against predate them, and the ABI is three calls and a handle. */
typedef void *UC_HPCON;
typedef HRESULT (WINAPI *pCreatePseudoConsole_t)(COORD, HANDLE, HANDLE, DWORD,
                                                 UC_HPCON *);
typedef HRESULT (WINAPI *pResizePseudoConsole_t)(UC_HPCON, COORD);
typedef void    (WINAPI *pClosePseudoConsole_t)(UC_HPCON);

static pCreatePseudoConsole_t p_CreatePseudoConsole;
static pResizePseudoConsole_t p_ResizePseudoConsole;
static pClosePseudoConsole_t  p_ClosePseudoConsole;
static int g_conpty_probed;

static void probe_conpty(void)
{
    HMODULE k32;
    if (g_conpty_probed) return;
    g_conpty_probed = 1;
    k32 = GetModuleHandleA("kernel32.dll");
    if (!k32) return;
    p_CreatePseudoConsole = (pCreatePseudoConsole_t)(void *)
        GetProcAddress(k32, "CreatePseudoConsole");
    p_ResizePseudoConsole = (pResizePseudoConsole_t)(void *)
        GetProcAddress(k32, "ResizePseudoConsole");
    p_ClosePseudoConsole = (pClosePseudoConsole_t)(void *)
        GetProcAddress(k32, "ClosePseudoConsole");
}

struct uc_proc {
    HANDLE in_w, out_r;          /* our ends                                */
    HANDLE proc;
    UC_HPCON pcon;               /* 0 when running on plain pipes           */
    PROCESS_INFORMATION pi;
    int exited, code;
};

int uc_proc_available(void) { return 1; }

const char *uc_proc_shell_name(void)
{
    probe_conpty();
    return p_CreatePseudoConsole ? "cmd.exe (ConPTY)" : "cmd.exe (pipes)";
}

uc_proc *uc_proc_spawn(const char *cmdline, const char *cwd)
{
    uc_proc *p;
    HANDLE in_r = 0, in_w = 0, out_r = 0, out_w = 0;
    SECURITY_ATTRIBUTES sa;
    STARTUPINFOEXA si;
    char cmd[2048];
    int ok = 0;

    g_err[0] = 0;
    probe_conpty();
    p = (uc_proc *)calloc(1, sizeof *p);
    if (!p) { snprintf(g_err, sizeof g_err, "out of memory"); return 0; }

    memset(&sa, 0, sizeof sa);
    sa.nLength = sizeof sa;
    sa.bInheritHandle = TRUE;
    if (!CreatePipe(&in_r, &in_w, &sa, 0) || !CreatePipe(&out_r, &out_w, &sa, 0)) {
        snprintf(g_err, sizeof g_err, "could not create the pipes");
        goto fail;
    }
    /* our ends must NOT be inherited, or the child holds them open and a read
     * after the child exits never reports end of file */
    SetHandleInformation(in_w, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(out_r, HANDLE_FLAG_INHERIT, 0);

    if (cmdline && cmdline[0])
        snprintf(cmd, sizeof cmd, "cmd.exe /c %s", cmdline);
    else
        snprintf(cmd, sizeof cmd, "cmd.exe");

    memset(&si, 0, sizeof si);
    si.StartupInfo.cb = sizeof si;

    if (p_CreatePseudoConsole) {
        COORD size;
        SIZE_T bytes = 0;
        size.X = 120; size.Y = 30;
        if (p_CreatePseudoConsole(size, in_r, out_w, 0, &p->pcon) != S_OK) {
            p->pcon = 0;
        } else {
            InitializeProcThreadAttributeList(0, 1, 0, &bytes);
            si.lpAttributeList = (LPPROC_THREAD_ATTRIBUTE_LIST)malloc(bytes);
            if (si.lpAttributeList &&
                InitializeProcThreadAttributeList(si.lpAttributeList, 1, 0, &bytes) &&
                UpdateProcThreadAttribute(si.lpAttributeList, 0,
                    (DWORD_PTR)0x00020016 /* PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE */,
                    p->pcon, sizeof p->pcon, 0, 0)) {
                ok = CreateProcessA(0, cmd, 0, 0, FALSE,
                                    EXTENDED_STARTUPINFO_PRESENT, 0, cwd,
                                    &si.StartupInfo, &p->pi) != 0;
            }
        }
    }
    if (!ok) {
        /* pipes: no ConPTY, or it refused.  Still a working terminal. */
        if (p->pcon && p_ClosePseudoConsole) { p_ClosePseudoConsole(p->pcon); p->pcon = 0; }
        si.StartupInfo.cb = sizeof si.StartupInfo;
        si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
        si.StartupInfo.hStdInput = in_r;
        si.StartupInfo.hStdOutput = out_w;
        si.StartupInfo.hStdError = out_w;
        ok = CreateProcessA(0, cmd, 0, 0, TRUE, CREATE_NO_WINDOW, 0, cwd,
                            &si.StartupInfo, &p->pi) != 0;
    }
    if (si.lpAttributeList) {
        DeleteProcThreadAttributeList(si.lpAttributeList);
        free(si.lpAttributeList);
    }
    if (!ok) {
        snprintf(g_err, sizeof g_err, "could not start cmd.exe (error %lu)",
                 (unsigned long)GetLastError());
        goto fail;
    }
    /* the child owns these ends now */
    CloseHandle(in_r);
    CloseHandle(out_w);
    p->in_w = in_w;
    p->out_r = out_r;
    p->proc = p->pi.hProcess;
    return p;

fail:
    if (in_r) CloseHandle(in_r);
    if (in_w) CloseHandle(in_w);
    if (out_r) CloseHandle(out_r);
    if (out_w) CloseHandle(out_w);
    free(p);
    return 0;
}

int uc_proc_read(uc_proc *p, char *buf, int cap)
{
    DWORD avail = 0, got = 0;
    if (!p || cap <= 0) return -1;
    if (p->out_r && PeekNamedPipe(p->out_r, 0, 0, 0, &avail, 0) && avail) {
        if ((int)avail > cap) avail = (DWORD)cap;
        if (ReadFile(p->out_r, buf, avail, &got, 0) && got) return (int)got;
    }
    if (!uc_proc_alive(p)) return -1;      /* ended AND nothing left to read */
    return 0;
}

int uc_proc_write(uc_proc *p, const char *s, int n)
{
    DWORD put = 0;
    if (!p || !p->in_w || n <= 0) return 0;
    if (!WriteFile(p->in_w, s, (DWORD)n, &put, 0)) return 0;
    return (int)put;
}

void uc_proc_resize(uc_proc *p, int cols, int rows)
{
    COORD c;
    if (!p || !p->pcon || !p_ResizePseudoConsole) return;
    c.X = (SHORT)(cols > 0 ? cols : 80);
    c.Y = (SHORT)(rows > 0 ? rows : 25);
    p_ResizePseudoConsole(p->pcon, c);
}

void uc_proc_interrupt(uc_proc *p)
{
    if (!p) return;
    /* Ctrl+C as a BYTE: the console subsystem turns it into the signal, and
     * GenerateConsoleCtrlEvent from a GUI process with no console of its own
     * reaches nobody. */
    uc_proc_write(p, "\003", 1);
}

int uc_proc_alive(uc_proc *p)
{
    DWORD code = 0;
    if (!p || !p->proc) return 0;
    if (p->exited) return 0;
    if (GetExitCodeProcess(p->proc, &code) && code == STILL_ACTIVE) return 1;
    p->exited = 1;
    p->code = (int)code;
    return 0;
}

int uc_proc_exit_code(uc_proc *p) { return p ? p->code : -1; }

void uc_proc_free(uc_proc *p)
{
    if (!p) return;
    if (uc_proc_alive(p)) TerminateProcess(p->proc, 1);
    if (p->pcon && p_ClosePseudoConsole) p_ClosePseudoConsole(p->pcon);
    if (p->in_w) CloseHandle(p->in_w);
    if (p->out_r) CloseHandle(p->out_r);
    if (p->pi.hThread) CloseHandle(p->pi.hThread);
    if (p->pi.hProcess) CloseHandle(p->pi.hProcess);
    free(p);
}

/* ======================================================================== */
#else   /* POSIX */
/* ======================================================================== */
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <errno.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <sys/wait.h>

#ifdef __APPLE__
#  include <util.h>
#else
#  include <pty.h>
#endif

struct uc_proc {
    int fd;                       /* the pty master                         */
    int pid;
    int exited, code;
};

int uc_proc_available(void) { return 1; }

const char *uc_proc_shell_name(void)
{
    const char *sh = getenv("SHELL");
    return (sh && sh[0]) ? sh : "/bin/sh";
}

uc_proc *uc_proc_spawn(const char *cmdline, const char *cwd)
{
    uc_proc *p;
    int fd = -1, pid;
    struct winsize ws;

    g_err[0] = 0;
    p = (uc_proc *)calloc(1, sizeof *p);
    if (!p) { snprintf(g_err, sizeof g_err, "out of memory"); return 0; }

    ws.ws_col = 120; ws.ws_row = 30; ws.ws_xpixel = 0; ws.ws_ypixel = 0;
    pid = forkpty(&fd, 0, 0, &ws);
    if (pid < 0) {
        snprintf(g_err, sizeof g_err, "could not start a terminal: %s",
                 strerror(errno));
        free(p);
        return 0;
    }
    if (pid == 0) {
        /* the child.  Nothing here may return: an exec that failed has to
         * _exit, or a second copy of the editor keeps running on this pty. */
        const char *sh = uc_proc_shell_name();
        /* RESET THE SIGNALS FIRST.  An IGNORED disposition survives exec, and
         * the editor may well be ignoring SIGINT without knowing it - a
         * process started in the background by a non-interactive shell has
         * SIGINT and SIGQUIT set to SIG_IGN, and hands that to everything it
         * spawns.  The symptom is a child that cannot be interrupted by
         * anything: not Ctrl+C, not a signal sent by hand, because the child
         * is ignoring the signal rather than missing it.  Every terminal
         * emulator does this; ours has to as well. */
        {
            struct sigaction sa;
            sigset_t none;
            memset(&sa, 0, sizeof sa);
            sa.sa_handler = SIG_DFL;
            sigaction(SIGINT,  &sa, 0);
            sigaction(SIGQUIT, &sa, 0);
            sigaction(SIGTERM, &sa, 0);
            sigaction(SIGHUP,  &sa, 0);
            sigaction(SIGPIPE, &sa, 0);
            sigaction(SIGCHLD, &sa, 0);
            sigemptyset(&none);
            sigprocmask(SIG_SETMASK, &none, 0);
        }
        if (cwd && cwd[0]) { if (chdir(cwd) != 0) { /* start where we are */ } }
        setenv("TERM", "xterm-256color", 1);
        if (cmdline && cmdline[0]) execl(sh, sh, "-c", cmdline, (char *)0);
        else execl(sh, sh, "-i", (char *)0);
        _exit(127);
    }
    /* the parent.  O_NONBLOCK is the whole reason this can be polled from a
     * frame loop rather than a thread. */
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
    p->fd = fd;
    p->pid = pid;
    return p;
}

int uc_proc_read(uc_proc *p, char *buf, int cap)
{
    int n;
    if (!p || cap <= 0) return -1;
    n = (int)read(p->fd, buf, (size_t)cap);
    if (n > 0) return n;
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        /* nothing waiting.  The child may still have ended - a pty master
         * reports EAGAIN, not EOF, until the slave side is fully closed. */
        return uc_proc_alive(p) ? 0 : -1;
    }
    /* 0 (EOF) or a real error: the child is done with this terminal */
    uc_proc_alive(p);
    return -1;
}

int uc_proc_write(uc_proc *p, const char *s, int n)
{
    int put;
    if (!p || n <= 0) return 0;
    put = (int)write(p->fd, s, (size_t)n);
    return put > 0 ? put : 0;
}

void uc_proc_resize(uc_proc *p, int cols, int rows)
{
    struct winsize ws;
    if (!p) return;
    ws.ws_col = (unsigned short)(cols > 0 ? cols : 80);
    ws.ws_row = (unsigned short)(rows > 0 ? rows : 25);
    ws.ws_xpixel = ws.ws_ypixel = 0;
    ioctl(p->fd, TIOCSWINSZ, &ws);
}

void uc_proc_interrupt(uc_proc *p)
{
    if (!p) return;
    /* the byte, not kill(): the pty's line discipline turns it into SIGINT
     * for the whole FOREGROUND GROUP, which is what reaches the compiler a
     * shell is waiting on rather than only the shell */
    uc_proc_write(p, "\003", 1);
}

int uc_proc_alive(uc_proc *p)
{
    int st = 0, r;
    if (!p || p->pid <= 0) return 0;
    if (p->exited) return 0;
    r = (int)waitpid(p->pid, &st, WNOHANG);
    if (r == 0) return 1;
    p->exited = 1;
    if (r > 0) {
        if (WIFEXITED(st)) p->code = WEXITSTATUS(st);
        else if (WIFSIGNALED(st)) p->code = 128 + WTERMSIG(st);
    } else p->code = -1;
    return 0;
}

int uc_proc_exit_code(uc_proc *p) { return p ? p->code : -1; }

void uc_proc_free(uc_proc *p)
{
    if (!p) return;
    if (uc_proc_alive(p)) {
        kill(p->pid, SIGHUP);          /* the polite one first */
        kill(p->pid, SIGKILL);
        { int st; waitpid(p->pid, &st, 0); }
    }
    if (p->fd >= 0) close(p->fd);
    free(p);
}

#endif
