#include <ucontext.h>
#include <stdlib.h>
#include <stdio.h>

#define STACK_SIZE (64 * 1024)

extern ucontext_t scheduler_ctx;
extern void encolar_hilo(struct Scheduler *sched, struct TCB *t);
extern int next_tid;
void thread_trampoline(void (*funcion)(void*), void *arg);

int my_thread_create(
    void (*funcion)(void*),
    void *arg,
    struct Scheduler *sched,
    int tickets,
    int priority,
    long deadline
) {
    struct TCB *hilo = malloc(sizeof(*t));
    if (hilo == NULL) return -1;

    if (getcontext(&hilo->context) == -1) {
        free(hilo);
        return -1;
    }

    hilo->stack = malloc(STACK_SIZE);
    if (hilo->stack==NULL) {
        free(hilo);
        return -1;
    }
    hilo->context.uc_stack.ss_sp= hilo->stack;
    hilo->context.uc_stack.ss_size = STACK_SIZE;

    hilo->context.uc_link = &scheduler_ctx;

    makecontext(&hilo->context,
                (void(*)(void))thread_trampoline,
                2, funcion, arg);

    hilo->tid       = next_tid++;
    hilo->estado     = READY;
    hilo->scheduler = sched;
    hilo->next      = NULL;
    hilo->tickets   = tickets;
    hilo->priority  = priority;
    hilo->deadline  = deadline;

    encolar_hilo(sched, hilo);

    return hilo->tid;
}

extern TCB *hilo_actual;
extern void schedule(void);

void my_thread_end(void) {
    TCB *self = hilo_actual;
    self->estado = TERMINATED;
    free(self->stack);
    self->stack = NULL;
    schedule();
    abort();
}

void my_thread_yield(void) {
    TCB *self = hilo_actual;
    self->estado = READY;
    encolar_hilo(self->scheduler, self);
    schedule();
}

extern TCB *buscar_hilo_id(int tid);

void my_thread_join(int tid) {
    TCB *self   = hilo_actual;
    TCB *hilo_encontrado = buscar_hilo_id(tid);

    if (hilo_encontrado == NULL || hilo_encontrado->estado == TERMINATED) {
        return;
    }

    if (hilo_encontrado == self) {
        return;
    }

    self->estado        = BLOCKED;
    hilo_encontrado->joiner = self;

    schedule();
}

