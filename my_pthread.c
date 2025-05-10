#define _POSIX_C_SOURCE 200809L

#include <ucontext.h>
#include <stdlib.h>
#include "my_pthread.h"
#define STACK_SIZE (64 * 1024)

extern ucontext_t scheduler_ctx;
extern ThreadPool   global_thread_pool;
extern TCB         *hilo_actual;
extern int          next_tid;


static void thread_trampoline(void (*funcion)(void*), void *arg) {
    funcion(arg);
    my_thread_end();
}

int my_thread_create(
    void (*funcion)(void*),
    void *arg,
    Scheduler *sched,
    int tickets,
    int priority,
    long deadline
) {
    TCB *hilo = malloc(sizeof *hilo);
    if (!hilo) return -1;

    if (getcontext(&hilo->context) == -1) {
        free(hilo);
        return -1;
    }

    hilo->stack = malloc(STACK_SIZE);
    if (!hilo->stack) {
        free(hilo);
        return -1;
    }
    hilo->context.uc_stack.ss_sp   = hilo->stack;
    hilo->context.uc_stack.ss_size = STACK_SIZE;
    hilo->context.uc_link          = &scheduler_ctx;

    makecontext(&hilo->context,
                (void(*)(void))thread_trampoline,
                2, funcion, arg);

    hilo->tid       = next_tid++;
    hilo->state     = READY;
    hilo->scheduler = sched;
    hilo->next      = NULL;
    hilo->tickets   = tickets;
    hilo->priority  = priority;
    hilo->deadline  = deadline;
    hilo->joiner    = NULL;
    hilo->detached  = 0;

    registrar_hilo(&global_thread_pool, hilo);
    encolar_hilo(sched, hilo);
    return hilo->tid;
}

void my_thread_end(void) {
    TCB *self = hilo_actual;
    self->state = TERMINATED;

    if (self->joiner) {
        self->joiner->state = READY;
        encolar_hilo(self->scheduler, self->joiner);
    }
    free(self->stack);

    if (self->detached || self->joiner == NULL) {
        free(self);
        schedule();
        abort();
    }

    schedule();
    abort();
}

void my_thread_yield(void) {
    TCB *self = hilo_actual;
    self->state = READY;
    encolar_hilo(self->scheduler, self);
    schedule();
}

void my_thread_join(int tid) {
    TCB *self = hilo_actual;
    TCB *h = buscar_hilo_id(&global_thread_pool, tid);
    if (!h || h->state == TERMINATED || h == self) return;
    self->state   = BLOCKED;
    h->joiner     = self;
    schedule();
}

int my_thread_detach(int tid) {
    TCB *h = buscar_hilo_id(&global_thread_pool, tid);
    if (!h) return -1;
    h->detached = 1;
    return 0;
}

