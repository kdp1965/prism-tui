/*
========================================================================
simevent.h: TinyQV port of the CSimEvent class (original: POSIX
semaphores).

Single threaded build: every consumer of these events lives in the one
main loop, so signalling is vacuous.  Trigger/Wait/Acquire/Release are
no-ops and TryWait always reports "nothing pending".  (The cross-thread
Request* paths in CTui that would genuinely block are never exercised -
the sources run inside RunThread itself.)
========================================================================
*/

#pragma once

class CSimEvent
{
public:
    CSimEvent() {}
    virtual ~CSimEvent() {}

    int     Open(void) { return 0; }
    void    Close(void) {}

    void    Trigger(void) {}
    void    Wait(void) {}
    int     TryWait(void) { return -1; }

    void    Release(void) {}
    void    Acquire(void) {}
};
