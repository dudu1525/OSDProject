#include "HAL9000.h"
#include "ex_timer.h"
#include "iomu.h"
#include "thread_internal.h"
#include "log.h"

typedef struct _GLOBAL_TIMER_LIST
{
    // protect the global timer list
    LOCK        TimerListLock;
    // the list's head
    LIST_ENTRY  TimerListHead;
} GLOBAL_TIMER_LIST, *PGLOBAL_TIMER_LIST;

static GLOBAL_TIMER_LIST m_globalTimerList;

/* GOG
Compara doua elemente de tip EX_TIMER in functie de TriggerTimeUs
*/
static
INT64
(CDECL ExTimerCompareListElems)
    (
    IN      PLIST_ENTRY     FirstElem,
    IN      PLIST_ENTRY     SecondElem,
    IN_OPT  PVOID           Context
    )
{
    PEX_TIMER firstTimer;
    PEX_TIMER secondTimer;

    UNREFERENCED_PARAMETER(Context);

    firstTimer  = CONTAINING_RECORD(FirstElem,  EX_TIMER, TimerListElem);
    secondTimer = CONTAINING_RECORD(SecondElem, EX_TIMER, TimerListElem);

    return firstTimer->TriggerTimeUs - secondTimer->TriggerTimeUs;
}

/* GOG
Initializeaza GLOBAL_TIMER_LIST
Inainte ca un timer sa fie folosit
*/
void ExTimerSystemPreinit(void)
{
    memzero(&m_globalTimerList, sizeof(GLOBAL_TIMER_LIST));
    LockInit(&m_globalTimerList.TimerListLock);
    LockSetName(&m_globalTimerList.TimerListLock, "TimerList");
    InitializeListHead(&m_globalTimerList.TimerListHead);
}

STATUS
ExTimerInit(
    OUT     PEX_TIMER       Timer,
    IN      EX_TIMER_TYPE   Type,
    IN      QWORD           Time
    )
{
    STATUS status;

    if (NULL == Timer)
    {
        return STATUS_INVALID_PARAMETER1;
    }

    if (Type > ExTimerTypeMax)
    {
        return STATUS_INVALID_PARAMETER2;
    }

    status = STATUS_SUCCESS;

    memzero(Timer, sizeof(EX_TIMER));

    Timer->Type = Type;
    if (Timer->Type != ExTimerTypeAbsolute)
    {
        // relative time

        // if the time trigger time has already passed the timer will
        // be signaled after the first scheduler tick
        Timer->TriggerTimeUs = IomuGetSystemTimeUs() + Time;
        Timer->ReloadTimeUs = Time;
    }
    else
    {
        // absolute
        Timer->TriggerTimeUs = Time;
    }

    /* GOG
    Initializare a LIST_ENTRY pentru inserarea timerului in lista globala de timere
    ExEventInit pentru sincronizarea threadurilor care asteapta timerul
    */
    InitializeListHead(&Timer->TimerListElem);
    status = ExEventInit(&Timer->TimerEvent, ExEventTypeNotification, FALSE);
    return status;
}

void
ExTimerStart(
    IN      PEX_TIMER       Timer
    )
{
    // INSTR_STATE e boolean : Interrupts enabled/disabled -GOG
    INTR_STATE oldState;

    ASSERT(Timer != NULL);

    if (Timer->TimerUninited)
    {
        return;
    }

    Timer->TimerStarted = TRUE;

    //Semnal in cazul in care timerul a expirat deja - GOG
    if (IomuGetSystemTimeUs() >= Timer->TriggerTimeUs)
    {
        ExEventSignal(&Timer->TimerEvent);
    }
    else
    {
        // Introducere in lista globala de timere - GOG
        LockAcquire(&m_globalTimerList.TimerListLock, &oldState);
        if (IsListEmpty(&Timer->TimerListElem))
        {
            InsertOrderedList(&m_globalTimerList.TimerListHead, &Timer->TimerListElem, ExTimerCompareListElems, NULL);
        }
        LockRelease(&m_globalTimerList.TimerListLock, oldState);
    }
}

void
ExTimerStop(
    IN      PEX_TIMER       Timer
    )
{
    // INSTR_STATE e boolean : Interrupts enabled/disabled -GOG
    INTR_STATE oldState;

    ASSERT(Timer != NULL);

    if (Timer->TimerUninited)
    {
        return;
    }

    Timer->TimerStarted = FALSE;

    //Timerul inactiv este eliminat din lista globala de timere - GOG
    LockAcquire(&m_globalTimerList.TimerListLock, &oldState);
    if (!IsListEmpty(&Timer->TimerListElem))
    {
        RemoveEntryList(&Timer->TimerListElem);
        InitializeListHead(&Timer->TimerListElem);
    }
    LockRelease(&m_globalTimerList.TimerListLock, oldState);

    //Trezeste threadurile care asteapta timerul - GOG
    ExEventSignal(&Timer->TimerEvent);
}

void
ExTimerWait(
    INOUT   PEX_TIMER       Timer
    )
{
    ASSERT(Timer != NULL);

    if (Timer->TimerUninited)
    {
        return;
    }

    /* Busy Wait
    while (IomuGetSystemTimeUs() < Timer->TriggerTimeUs && Timer->TimerStarted)
    {
        ThreadYield();
    }
    */
   
    // Asteapta semnalul de la timer - GOG
    ExEventWaitForSignal(&Timer->TimerEvent);
}

void
ExTimerUninit(
    INOUT   PEX_TIMER       Timer
    )
{
    ASSERT(Timer != NULL);

    ExTimerStop(Timer);

    Timer->TimerUninited = TRUE;

    // Distruge evenimentul asociat timerului - GOG
    // Cand e dezinitializat
    ExEventDestroy(&Timer->TimerEvent);
}

INT64
ExTimerCompareTimers(
    IN      PEX_TIMER     FirstElem,
    IN      PEX_TIMER     SecondElem
)
{
    return FirstElem->TriggerTimeUs - SecondElem->TriggerTimeUs;
}

// Se ocupa de timerele expirate - GOG
static void ExTimerCheck(
    IN      PEX_TIMER      Timer
)
{
    if (Timer->Type == ExTimerTypeRelativePeriodic)
    {
        // Timerul periodic este semnalat 
        // si reintrodus in lista globala de timere - GOG
        ExEventSignal(&Timer->TimerEvent);    
        ExEventClearSignal(&Timer->TimerEvent);

        RemoveEntryList(&Timer->TimerListElem);
        Timer->TriggerTimeUs += Timer->ReloadTimeUs;
        InsertOrderedList(&m_globalTimerList.TimerListHead, &Timer->TimerListElem, ExTimerCompareListElems, NULL);
    }
    else
    {
        // Timerul a expirat, este semnalat si eliminat din lista globala de timere - GOG
        RemoveEntryList(&Timer->TimerListElem);
        InitializeListHead(&Timer->TimerListElem);

        ExEventSignal(&Timer->TimerEvent);
        Timer->TimerStarted = FALSE;
    }
}

// Verifica timerele din lista globala si le semnaleaza pe cele expirate - GOG 
void ExTimerCheckAll(void)
{
    INTR_STATE oldState;
    PLIST_ENTRY pEntry;
    PEX_TIMER   pTimer;
    QWORD       currentTimeUs;

    currentTimeUs = IomuGetSystemTimeUs();

    LockAcquire(&m_globalTimerList.TimerListLock, &oldState);

    // Flink este pointerul catre urmatorul element din lista - GOG
    pEntry = m_globalTimerList.TimerListHead.Flink;

    while (pEntry != &m_globalTimerList.TimerListHead)
    {
        pTimer = CONTAINING_RECORD(pEntry, EX_TIMER, TimerListElem);

        // Lista fiind ordonata se opreste la primul timer care nu a expirat - GOG
        if (currentTimeUs < pTimer->TriggerTimeUs)
        {
            break;
        }

        pEntry = pEntry->Flink;

        // Daca timerul este activ, proceseaza expirarea lui - GOG
        if (pTimer->TimerStarted)
        {
            ExTimerCheck(pTimer);
        }
    }
    LockRelease(&m_globalTimerList.TimerListLock, oldState);
}