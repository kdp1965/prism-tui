// Host filesystem client.  See tqv_fs.h for the wire format.

#include <string.h>
#include <stdio.h>
#include <fcntl.h>

#include "tqv_fs.h"
#include "uart.h"
#include "csr.h"

// The frame is wrapped in an APC string (ESC _ ... ESC \).  tqv.py
// strips it either way, but a design talking to a plain terminal still
// probes once per boot - and terminals discard APC strings silently, so
// that probe leaves nothing on screen instead of spelling "TQVFSPING"
// at the user.  Everything, payload included, sits inside the wrapper.
#define FS_MARK         "\x1b_\x05TQVFS\x05"
#define FS_MARK_LEN     (sizeof(FS_MARK) - 1)
#define FS_TERM         "\x1b\\"
#define FS_TERM_LEN     (sizeof(FS_TERM) - 1)
#define FS_RESP_MARK    0x06
#define FS_ESC          0x1A            // payload escape (see fs_response)

// Per-byte deadlines.  Generous for a served request (the host may be
// hitting a cold disk), short for the probe so a design on a plain
// terminal barely notices the one lost round trip.
#define FS_TIMEOUT_US   2000000u
#define FS_PROBE_US     300000u

// Bound the hunt for a response marker so stray console traffic can
// never wedge us in a read loop.
#define FS_SKIP_MAX     4096

static int8_t s_probed;         // 0 unknown, 1 connected, -1 no host
static char   s_host[40];

// --------------------------------------------------------------------------
// Wire I/O.  uart_put_buffer() turns '\n' into "\r\n", which would
// corrupt every binary payload, so writes go a byte at a time through
// uart_putc().  printf() is off limits too: a full-screen UI hooks
// stdout (see __tinyqv_stdout_hook), and these bytes must reach the
// host verbatim.
// --------------------------------------------------------------------------
static void fs_raw(const void *data, int len)
{
    const unsigned char *p = (const unsigned char *)data;

    while (len-- > 0)
        uart_putc(*p++);
}

static int fs_getc(uint32_t timeout_us)
{
    uint32_t start = read_time();
    int c;

    for (;;) {
        c = uart_getc();
        if (c != -1)
            return c;
        if ((uint32_t)(read_time() - start) > timeout_us)
            return -1;
    }
}

// Drop anything sitting in the receive ring before a request goes out.
// It can only be type-ahead or a late reply to a probe we already gave
// up on, and either one would be parsed as this request's response.
// Losing a keystroke typed in the moment before a file operation is a
// far better trade than a desynchronised transaction.
static void fs_drain(void)
{
    while (uart_getc() != -1)
        ;
}

static void fs_request(const char *line, const void *payload, int paylen)
{
    fs_drain();
    fs_raw(FS_MARK, FS_MARK_LEN);
    fs_raw(line, (int)strlen(line));
    fs_raw("\n", 1);
    if (paylen > 0)
        fs_raw(payload, paylen);
    fs_raw(FS_TERM, FS_TERM_LEN);
}

static int fs_hex(int c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

// Collect a response into buf.  Returns the payload length on success,
// -1 on timeout or a malformed frame, -2 when the host reported an
// error (buf then holds its message).  Payload bytes past buflen are
// read and dropped so the stream stays in sync.
static int fs_response(char *buf, int buflen, uint32_t timeout_us)
{
    int c, i, len = 0, status;

    for (i = 0; i < FS_SKIP_MAX; ++i) {
        c = fs_getc(timeout_us);
        if (c < 0)
            return -1;
        if (c == FS_RESP_MARK)
            break;
    }
    if (i >= FS_SKIP_MAX)
        return -1;

    status = fs_getc(timeout_us);
    if (status < 0)
        return -1;

    for (i = 0; i < 4; ++i) {
        int d = fs_hex(fs_getc(timeout_us));

        if (d < 0)
            return -1;
        len = (len << 4) | d;
    }
    if (fs_getc(timeout_us) != '\n')
        return -1;

    // Payload bytes arrive escaped where the RP2350 bridge would
    // otherwise eat them (0x11 is Ctrl-Q to it, and 0x11 'q' stops the
    // design outright): 0x1a followed by the byte plus 0x20.  len counts
    // DECODED bytes.
    for (i = 0; i < len; ++i) {
        c = fs_getc(timeout_us);
        if (c < 0)
            return -1;
        if (c == FS_ESC) {
            c = fs_getc(timeout_us);
            if (c < 0)
                return -1;
            c = (c - 0x20) & 0xFF;
        }
        if (i < buflen)
            buf[i] = (char)c;
    }
    if (len > buflen)
        len = buflen;

    return status == '+' ? len : -2;
}

// Parse a decimal payload ("12", "-1"); returns def on anything else
static long fs_num(const char *buf, int len, long def)
{
    long v = 0;
    int i = 0, neg = 0;

    if (len <= 0)
        return def;
    if (buf[0] == '-') {
        neg = 1;
        i = 1;
    }
    if (i >= len)
        return def;
    for (; i < len; ++i) {
        if (buf[i] < '0' || buf[i] > '9')
            return def;
        v = v * 10 + (buf[i] - '0');
    }
    return neg ? -v : v;
}

// --------------------------------------------------------------------------
// Probe
// --------------------------------------------------------------------------
void tqv_fs_reprobe(void)
{
    char buf[sizeof(s_host)];
    int  n;

    s_host[0] = 0;
    s_probed = -1;

    fs_request("PING", NULL, 0);
    n = fs_response(buf, (int)sizeof(buf) - 1, FS_PROBE_US);
    if (n >= 0) {
        buf[n] = 0;
        snprintf(s_host, sizeof(s_host), "%s", buf);
        s_probed = 1;
    }
}

bool tqv_fs_available(void)
{
    if (s_probed == 0)
        tqv_fs_reprobe();
    return s_probed > 0;
}

bool tqv_fs_probe_wait(uint32_t window_ms)
{
    uint32_t start = read_time();

    // Startup probe.  Straight out of reset the design is talking to
    // whatever the host has ready, and tqv.py does not enter its console
    // loop (where requests are served) until it has finished starting
    // the design - so the first PING or two can go unanswered even
    // though a server is coming.  Retry across a short window rather
    // than deciding "no filesystem" on the strength of one attempt.
    for (;;) {
        tqv_fs_reprobe();
        if (s_probed > 0)
            return true;
        if ((uint32_t)(read_time() - start) / 1000u >= window_ms)
            return false;
    }
}

const char *tqv_fs_host(void)
{
    return s_host;
}

// --------------------------------------------------------------------------
// Operations
// --------------------------------------------------------------------------
int tqv_fs_open(const char *path, const char *mode)
{
    char line[144], buf[24];
    int  n;

    if (!tqv_fs_available())
        return -1;

    snprintf(line, sizeof(line), "OPEN %s %s", mode, path);
    fs_request(line, NULL, 0);
    n = fs_response(buf, (int)sizeof(buf), FS_TIMEOUT_US);
    if (n < 0)
        return -1;
    return (int)fs_num(buf, n, -1);
}

int tqv_fs_close(int handle)
{
    char line[32], buf[64];

    if (!tqv_fs_available() || handle < 0)
        return -1;

    snprintf(line, sizeof(line), "CLOSE %d", handle);
    fs_request(line, NULL, 0);
    return fs_response(buf, (int)sizeof(buf), FS_TIMEOUT_US) < 0 ? -1 : 0;
}

int tqv_fs_read(int handle, void *buf, int len)
{
    char line[32];

    if (!tqv_fs_available() || handle < 0 || len <= 0)
        return -1;
    if (len > TQV_FS_CHUNK)
        len = TQV_FS_CHUNK;

    snprintf(line, sizeof(line), "READ %d %d", handle, len);
    fs_request(line, NULL, 0);
    return fs_response((char *)buf, len, FS_TIMEOUT_US);
}

int tqv_fs_write(int handle, const void *buf, int len)
{
    char line[32], rsp[24];
    int  n;

    if (!tqv_fs_available() || handle < 0 || len <= 0)
        return -1;
    if (len > TQV_FS_CHUNK)
        len = TQV_FS_CHUNK;

    snprintf(line, sizeof(line), "WRITE %d %d", handle, len);
    fs_request(line, buf, len);
    n = fs_response(rsp, (int)sizeof(rsp), FS_TIMEOUT_US);
    if (n < 0)
        return -1;
    return (int)fs_num(rsp, n, -1);
}

long tqv_fs_seek(int handle, long offset, int whence)
{
    char line[48], buf[24];
    int  n;

    if (!tqv_fs_available() || handle < 0)
        return -1;

    snprintf(line, sizeof(line), "SEEK %d %ld %d", handle, offset, whence);
    fs_request(line, NULL, 0);
    n = fs_response(buf, (int)sizeof(buf), FS_TIMEOUT_US);
    if (n < 0)
        return -1;
    return fs_num(buf, n, -1);
}

int tqv_fs_list(const char *dir, char *buf, int buflen)
{
    char line[144];

    if (!tqv_fs_available())
        return -1;

    snprintf(line, sizeof(line), "LIST %s", dir && *dir ? dir : ".");
    fs_request(line, NULL, 0);
    return fs_response(buf, buflen, FS_TIMEOUT_US);
}

// --------------------------------------------------------------------------
// newlib syscall side: runtime.c declares these weak and routes file
// descriptors >= TQV_FS_FD_BASE to them, so stdio just works.  A handle
// maps to a descriptor by adding the base.
// --------------------------------------------------------------------------
int __tinyqv_fs_open(const char *path, int flags)
{
    const char *mode;
    int handle;

    // Rebuild the fopen mode the host understands from the O_ bits
    if (flags & O_APPEND)
        mode = (flags & O_RDWR) ? "a+" : "a";
    else if (flags & O_TRUNC)
        mode = (flags & O_RDWR) ? "w+" : "w";
    else if ((flags & O_ACCMODE) == O_RDONLY)
        mode = "r";
    else if ((flags & O_ACCMODE) == O_WRONLY)
        mode = (flags & O_CREAT) ? "w" : "r+";
    else
        mode = (flags & O_CREAT) ? "w+" : "r+";

    handle = tqv_fs_open(path, mode);
    return handle < 0 ? -1 : handle + TQV_FS_FD_BASE;
}

int __tinyqv_fs_close(int fd)
{
    return tqv_fs_close(fd - TQV_FS_FD_BASE);
}

int __tinyqv_fs_read(int fd, char *buf, int len)
{
    return tqv_fs_read(fd - TQV_FS_FD_BASE, buf, len);
}

int __tinyqv_fs_write(int fd, const char *buf, int len)
{
    return tqv_fs_write(fd - TQV_FS_FD_BASE, buf, len);
}

long __tinyqv_fs_lseek(int fd, long offset, int whence)
{
    return tqv_fs_seek(fd - TQV_FS_FD_BASE, offset, whence);
}
