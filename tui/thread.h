/*
========================================================================
thread.h: TinyQV port of the CThread class (original: NuttX pthreads).

Bare metal has one hart and no scheduler: Start() simply runs the
thread body inline and returns when it finishes.  The TUI already
supported this - its Main called RunThread() directly instead of
Start() - so nothing here ever spawns.
========================================================================
*/

#pragma once

class CThread
{
public:
    CThread() {}
    virtual ~CThread() {}

    void            Start(void) { RunThread(); }
    virtual void    RunThread() = 0;
    virtual void    StopThread() {}
};
