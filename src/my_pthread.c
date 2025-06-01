#define _POSIX_C_SOURCE 200809L

#include <ucontext.h>
#include <stdlib.h>
#include "../include/my_pthread.h"



#include <stdio.h>
#define STACK_SIZE (64 * 1024)

extern ucontext_t scheduler_ctx;
extern ThreadPool   global_thread_pool;
extern TCB         *hilo_actual;
extern int          next_tid;




static void thread_trampoline(void (*funcion)(void*), void *arg) {
    funcion(arg);
    my_thread_end();
}

//Funcion encargada de crear nuevos hilos
int my_thread_create(
    void (*funcion)(void*),
    void *arg,
    Scheduler *sched,
    int tickets,
    int priority,
    long deadline
) {
    TCB *hilo = malloc(sizeof *hilo);
    if (hilo==NULL) return -1;

    if (getcontext(&hilo->context) == -1) {
        free(hilo);
        return -1;
    }

    hilo->stack = malloc(STACK_SIZE);
    if (hilo->stack == NULL) {
        free(hilo);
        return -1;
    }
    hilo->context.uc_stack.ss_sp = hilo->stack;
    hilo->context.uc_stack.ss_size = STACK_SIZE;
    hilo->context.uc_link = &scheduler_ctx;
    makecontext(&hilo->context,
            (void(*)(void))thread_trampoline,
            2, funcion, arg);

    hilo->tid = next_tid++;
    hilo->state = READY;
    hilo->scheduler = sched;
    hilo->next = NULL;
    hilo->tickets = tickets;
    hilo->priority = priority;
    hilo->deadline = deadline;
    hilo->joiner = NULL;
    hilo->detached = 0;

    registrar_hilo(&global_thread_pool, hilo);
    encolar_hilo(sched, hilo);



    return hilo->tid;
}




void my_thread_end(void) {
    TCB *actual = hilo_actual;
    actual->state = TERMINATED;
    schedule();
}



//Funcion encargada de ceder voluntariamente la CPU
void my_thread_yield(void) {
    TCB *actual = hilo_actual;
    actual->state = READY;
    encolar_hilo(actual->scheduler, actual);
    schedule();
}

//Funcion encargada de bloquear el hilo actual hasta que el hilo con identificador tid termine
void my_thread_join(int tid) {
    TCB *actual = hilo_actual;
    TCB *hilo_prioritario = buscar_hilo_id(&global_thread_pool, tid);
    if (hilo_prioritario == NULL || hilo_prioritario->state == TERMINATED || hilo_prioritario == actual) return;
    actual->state = BLOCKED;
    hilo_prioritario->joiner = actual;
    schedule();
}

//funcion encargada de amarcar el hilo como detached
int my_thread_detach(int tid) {
    TCB *hilo = buscar_hilo_id(&global_thread_pool, tid);
    if (hilo == NULL) return -1;
    hilo->detached = 1;
    return 0;
}

static void encolar_mutex(my_mutex *mutex, TCB *hilo) {
    hilo->next = NULL;
    if (mutex->head == NULL) {
        mutex->head = mutex->tail = hilo;
    }
    else {
        mutex->tail->next = hilo;
        //Ahora el tail en siguiente es el nuevo ultimo
        mutex->tail = hilo;
    }
}

static TCB *desencolar_mutex(my_mutex *mutex) {
    TCB *hilo = mutex->head;
    if (hilo == NULL) {
        return NULL;
    }
    mutex->head = hilo->next;
    if (mutex->head == NULL) {
        mutex->tail = NULL;
    }
    hilo->next = NULL;
    return hilo;
}

int my_mutex_init(my_mutex *mutex) {
    if (mutex == NULL) {
        return -1;
    }
    mutex->bloqueado = 0;
    mutex->propietario = NULL;
    mutex->head = NULL;
    mutex->tail = NULL;
    mutex->occupied_positions = NULL;
    return 0;
}

int my_mutex_destroy(my_mutex *mutex) {
    if (mutex  == NULL || mutex->bloqueado || mutex->head) {
        return -1;
    }
    mutex->bloqueado = 0;
    mutex->propietario  = NULL;
    mutex->head = NULL;
    mutex->tail = NULL;
    return 0;
}

int my_mutex_lock(my_mutex *mutex) {
    if (mutex == NULL) {
        return -1;
    }
    if (mutex->bloqueado == 0) {
        mutex->bloqueado = 1;
        mutex->propietario  = hilo_actual;
        return 0;
    }
    if (mutex->propietario == hilo_actual) {
        return -1;
    }

    //Si esta ocupado lo mete en la cola
    TCB *actual = hilo_actual;
    encolar_mutex(mutex, actual);
    actual->state = BLOCKED;
    schedule();
    return 0;
}

// intenta bloquearlo y si no puede no hace nada, no lo mete en la cola
int my_mutex_trylock(my_mutex *mutex) {
    if (mutex == NULL) {
        return -1;
    }
    if (mutex->bloqueado == 0) {
        mutex->bloqueado = 1;
        mutex->propietario  = hilo_actual;
        return 0;
    }
    return -1;
}

int my_mutex_unlock(my_mutex *mutex) {
    if (mutex == NULL || mutex->bloqueado == 0 || mutex->propietario != hilo_actual) {
        return -1;
    }
    // Si hay un hilo esperando se le da acceso al mutex
    TCB *siguiente = desencolar_mutex(mutex);
    if (siguiente != NULL) {
        siguiente->state = READY;
        encolar_hilo(siguiente->scheduler, siguiente);
        mutex->propietario = siguiente;

    } else {
        // No hay nadie esperando, solo se libera
        mutex->bloqueado = 0;
        mutex->propietario = NULL;
    }
    return 0;
}