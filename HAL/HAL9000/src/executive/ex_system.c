#include "HAL9000.h"
#include "ex_system.h"
#include "ex_timer.h"
#include "thread_internal.h"

void
ExSystemTimerTick(
    void
    )
{
    ExTimerCheckAll();
    ThreadTick();
}