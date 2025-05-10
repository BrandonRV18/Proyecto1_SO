#define _POSIX_C_SOURCE 200809L

#include "scheduler.h"
#include <stdlib.h>     // malloc, free, realloc
#include <signal.h>     // sigaction
#include <sys/time.h>   // setitimer, struct itimerval


ThreadPool   global_thread_pool = { 0, NULL, 0, 0 };
TCB         *hilo_actual        = NULL;
int          next_tid           = 0;
ucontext_t   scheduler_ctx;


static void ensure_capacity(ThreadPool *p) {
    if (p->count + 1 > p->capacity) {
        size_t new_cap = (p->capacity == 0 ? 4 : p->capacity * 2);
        p->threads = realloc(p->threads, new_cap * sizeof(TCB *));
        p->capacity = new_cap;
    }
}

int registrar_hilo(ThreadPool *p, TCB *t) {
    ensure_capacity(p);
    p->threads[p->count++] = t;
    return t->tid;
}

TCB *buscar_hilo_id(ThreadPool *p, int tid) {
    for (size_t i = 0; i < p->count; i++) {
        if (p->threads[i]->tid == tid) {
            return p->threads[i];
        }
    }
    return NULL;
}


void encolar_hilo(Scheduler *sched, TCB *t) {
    sched->encolar_hilo(sched, t);
}


void schedule(void) {
    if (hilo_actual == NULL) return;
    TCB *prev      = hilo_actual;
    Scheduler *sch = prev->scheduler;
    TCB *next      = sch->siguiente_hilo(sch);
    if (next == NULL) return;
    hilo_actual = next;
    swapcontext(&prev->context, &next->context);
}


static void alarm_handler(int sig) {
    (void)sig;
    schedule();
}

static void start_preemption(int quantum_ms) {
    struct sigaction sa;
    sigemptyset(&sa.sa_mask);
    sa.sa_handler = alarm_handler;
    sa.sa_flags   = SA_RESTART;
    sigaction(SIGALRM, &sa, NULL);

    struct itimerval timer = {
        .it_interval = { .tv_sec = quantum_ms/1000,
                         .tv_usec = (quantum_ms%1000)*1000 },
        .it_value    = { .tv_sec = quantum_ms/1000,
                         .tv_usec = (quantum_ms%1000)*1000 }
    };
    setitimer(ITIMER_REAL, &timer, NULL);
}


static void rr_encolar_hilo(Scheduler *s, TCB *t) {
    RR_Scheduler *rr = (RR_Scheduler*)s;
    t->scheduler     = s;
    t->state         = READY;
    t->next          = NULL;
    if (rr->tail == NULL) rr->head = t;
    else                  rr->tail->next = t;
    rr->tail = t;
}

static TCB *rr_siguiente_hilo(Scheduler *s) {
    RR_Scheduler *rr = (RR_Scheduler*)s;
    if (rr->head == NULL) return NULL;
    TCB *chosen = rr->head;
    rr->head    = chosen->next;
    if (rr->head == NULL) rr->tail = NULL;
    chosen->next  = NULL;
    chosen->state = RUNNING;
    if (rr->tail == NULL) rr->head = chosen;
    else                   rr->tail->next = chosen;
    rr->tail = chosen;
    return chosen;
}

void rr_scheduler_init(RR_Scheduler *rr, int quantum_ms) {
    rr->base.encolar_hilo   = rr_encolar_hilo;
    rr->base.siguiente_hilo = rr_siguiente_hilo;
    rr->quantum             = quantum_ms;
    rr->head = rr->tail     = NULL;
    start_preemption(quantum_ms);
}
