#pragma once

// Host filesystem access over the UART console.
//
// tqv.py's console watches the design's output for a framed request and
// serves it out of a directory on the host (its --fs-root, default
// tqvfs/ beside tqv.py), so a design can list, read and write real files
// with no storage of its own.  Nothing else on the link changes: the
// console strips the frames before the terminal sees them, and suspends
// keyboard forwarding for the microseconds a transaction takes.
//
// Wire format
// -----------
//   request   \x05TQVFS\x05<VERB> <args>\n[payload]      design -> host
//   response  \x06<status><4 hex length>\n<payload>      host -> design
//
// status '+' = ok (payload is the result), '-' = failed (payload is the
// message).  Lengths are hex so the design can parse them without
// pulling in scanf.  Verbs: PING, LIST, OPEN, CLOSE, READ, WRITE, SEEK.
//
// The marker follows the convention already used by the song-download
// hooks (\x05TQVRX\x05 and friends) in this SDK.
//
// Availability
// ------------
// A design may just as well be talking to a plain terminal (the Tiny
// Tapeout web console) with nothing to serve it.  tqv_fs_available()
// probes once - PING with a short deadline - and caches the answer, so
// every other call fails immediately instead of stalling.  On a plain
// terminal the probe leaves one stray "TQVFS PING" on screen and is
// never repeated unless tqv_fs_reprobe() asks for it (the 'fs' command
// in pwl-test does, for when tqv.py is attached later).
//
// Files also work through plain stdio - fopen/fgets/fprintf/fclose -
// because runtime.c routes file descriptors 3 and up here (weak
// symbols: apps that never link tqv_fs.c keep the old stubs).

#include <stdint.h>
#include <stdbool.h>

// Payload cap per read/write.  The hardware UART RX ring is only 64
// bytes, but the transfer loop does nothing except drain it, far faster
// than 115200 baud fills it.  1024 matches stdio's buffer, so a buffered
// fread costs one round trip instead of four - which is the difference
// between a MIDI file arriving in seconds and in a minute.
#define TQV_FS_CHUNK        1024

#define TQV_FS_MAX_HANDLES  8       // must match tqv.py
#define TQV_FS_FD_BASE      3       // fd 0/1/2 stay stdin/stdout/stderr

#ifdef __cplusplus
extern "C" {
#endif

// True when tqv.py is serving files (probes once, then cached)
bool tqv_fs_available(void);

// Force the next tqv_fs_available() to probe again
void tqv_fs_reprobe(void);

// Startup probe: retry for up to window_ms before giving up, because a
// host that is still starting the design cannot answer yet.  Returns
// what it settled on.
bool tqv_fs_probe_wait(uint32_t window_ms);

// Host description from the PING reply ("" when not connected)
const char *tqv_fs_host(void);

// mode is an fopen-style string: "r", "w", "a", "r+", "w+", "a+"
// (a trailing 'b' is accepted and ignored - everything is binary here).
// Returns a handle >= 0, or -1.
int  tqv_fs_open(const char *path, const char *mode);
int  tqv_fs_close(int handle);

// Short reads are normal (the payload cap); 0 means end of file
int  tqv_fs_read(int handle, void *buf, int len);
int  tqv_fs_write(int handle, const void *buf, int len);
long tqv_fs_seek(int handle, long offset, int whence);

// Directory listing into buf as "<f|d> <size> <name>\n" lines.
// Returns the byte count, or -1.
int  tqv_fs_list(const char *dir, char *buf, int buflen);

#ifdef __cplusplus
}
#endif
