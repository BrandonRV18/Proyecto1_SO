#ifndef SCHEDULER_H
#define SCHEDULER_H

#include <ucontext.h>
#include <stddef.h>



typedef enum {
    READY,
    RUNNING,
    BLOCKED,
    TERMINATED
} ThreadState;


typedef struct Scheduler    Scheduler;
typedef struct TCB          TCB;
typedef struct RR_Scheduler RR_Scheduler;
typedef struct Lottery_Scheduler Lottery_Scheduler;
typedef struct EDF_Scheduler EDF_Scheduler;


struct Scheduler {
    void   (*encolar_hilo)(Scheduler *self, TCB *t);
    TCB   *(*siguiente_hilo)(Scheduler *self);
    void   (*remover_hilo)   (Scheduler *self, TCB *t);
};


struct TCB {
    int               tid;
    ucontext_t        context;
    ThreadState       state;
    Scheduler        *scheduler;
    void             *stack;
    TCB              *next;
    int               tickets;
    int               priority;
    long              deadline;
    TCB              *joiner;
    int               detached;
};

struct RR_Scheduler {
    Scheduler base;
    int       quantum;
    TCB      *head;
    TCB      *tail;
};

struct Lottery_Scheduler {
    Scheduler base;
    TCB      *head;
};

struct EDF_Scheduler {
    Scheduler base;
    TCB      *head;
};

typedef struct {
    size_t created_threads_counter;
    TCB   **threads;
    size_t  count;
    size_t  capacity;
} ThreadPool;


extern ThreadPool   global_thread_pool;
extern TCB         *hilo_actual;
extern int          next_tid;
extern ucontext_t   scheduler_ctx;


int    registrar_hilo(ThreadPool *p, TCB *t);
int    my_thread_chsched(TCB *t, Scheduler *new_sch);
TCB   *buscar_hilo_id(ThreadPool *p, int tid);
void   encolar_hilo(Scheduler *sched, TCB *t);
void   schedule(void);

void   rr_scheduler_init(RR_Scheduler *rr, int quantum_ms);
void   lottery_scheduler_init(Lottery_Scheduler *ls);
void   edf_scheduler_init(EDF_Scheduler *es);

#endif
