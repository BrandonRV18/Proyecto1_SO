#define _POSIX_C_SOURCE 200809L
#include <ucontext.h>
#include <stdlib.h>
#include "../include/my_pthread.h"
#include <stdio.h>
#define STACK_SIZE (64 * 1024)

extern ucontext_t scheduler_ctx;
extern ThreadPool global_thread_pool;
extern TCB *hilo_actual;
extern int next_tid;

/**
 * pasar_funcion
 *
 * Función auxiliar que actúa como puente para arrancar la ejecución de un hilo.
 * Recibe un puntero a la función que el hilo debe ejecutar y su argumento.
 * Internamente invoca la función  y al finalizar, llama a my_thread_end().
 *
 * Entradas:
 *  - funcion: puntero a la función que ejecutará el hilo.
 *  - arg    : puntero al dato que se pasará a la función.
 *
 * Retorna:
 *  - void (no devuelve valor; al terminar, marca el hilo como terminado).
 */
static void pasar_funcion(void (*funcion)(void*), void *arg) {
    funcion(arg);
    my_thread_end();
}

/**
 * my_thread_create
 *
 * Crea un nuevo hilo con los parámetros y scheduler especificados. Reserva y
 * configura el TCB y la pila, inicializa el contexto para que arranque en
 * thread_trampoline(funcion, arg), le asigna un TID único, lo registra en el pool
 * global y lo encola en la cola de READY del scheduler. Si ocurre un error, retorna -1.
 *
 * Entradas:
 *  - funcion : puntero a la función que ejecutará el hilo.
 *  - arg     : puntero al argumento que se pasará a la función del hilo.
 *  - sched   : puntero al Scheduler.
 *  - tickets : número de tickets (para  lottery).
 *  - priority: prioridad del hilo (para  RMS, no se utilizó).
 *  - deadline: plazo límite de ejecución (para EDF).
 *
 * Retorna:
 *  - int: TID del hilo recién creado, o -1 si falla la creación.
 */
int my_thread_create( void (*funcion)(void*), void *arg, Scheduler *sched, int tickets, int priority, long deadline) {
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
            (void(*)(void))pasar_funcion,
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

/**
 * my_thread_end
 *
 * Marca el hilo actual (hilo_actual) como TERMINATED. Si existe un hilo que
 * llamó a join, lo desbloquea y lo encola nuevamente
 * en su scheduler. Finalmente, invoca schedule() para hacer el cambio entre hilos.
 *
 * Entradas:
 *  - Ninguna
 *
 * Retorna:
 *  - Ninguna
 */
void my_thread_end(void) {
    TCB *actual = hilo_actual;
    actual->state = TERMINATED;

    if (actual->joiner) {
        actual->joiner->state = READY;
        encolar_hilo(actual->scheduler, actual->joiner);
    }

    schedule();
}


/**
 * my_thread_yield
 *
 * Cede voluntariamente la CPU desde el hilo actual. Cambia su estado a READY,
 * lo encola de nuevo en la cola de su scheduler y llama a schedule() para
 * que otro hilo READY sea seleccionado para ejecutar.
 *
 * Entradas:
 *  - Ninguna
 *
 * Retorna:
 *  - Ninguna
 */
void my_thread_yield(void) {
    TCB *actual = hilo_actual;
    actual->state = READY;
    encolar_hilo(actual->scheduler, actual);
    schedule();
}

/**
 * my_thread_join
 *
 * Bloquea el hilo actual hasta que el hilo identificado por tid termine su
 * ejecución. Si el hilo objetivo no existe, ya está TERMINATED, es el mismo
 * hilo o está en modo detached, retorna sin bloquearse. En caso contrario,
 * marca el hilo actual como BLOCKED, asigna hilo_actual a joiner del nuevo hilo
 * y llama a schedule().
 *
 * Entradas:
 *  - tid: identificador del hilo que va a esperar.
 *
 * Retorna:
 *  - Ninguna
 */
void my_thread_join(int tid) {
    TCB *actual = hilo_actual;
    TCB *hilo_prioritario = buscar_hilo_id(&global_thread_pool, tid);
    if (hilo_prioritario == NULL || hilo_prioritario->state == TERMINATED || hilo_prioritario == actual ||
        hilo_prioritario->detached) return;
    actual->state = BLOCKED;
    hilo_prioritario->joiner = actual;
    schedule();
}

/**
 * my_thread_detach
 *
 * Marca el hilo con el identificador tid como detached. Si no existe un hilo con
 * ese TID, retorna -1, si sí existe, establece detached = 1 en su TCB.
 *
 * Entradas:
 *  - tid: identificador del hilo que va a poner en modo detached.
 *
 * Retorna:
 *  - int: 0 si no falló, -1 si no se encontró el hilo.
 */
int my_thread_detach(int tid) {
    TCB *hilo = buscar_hilo_id(&global_thread_pool, tid);
    if (hilo == NULL) return -1;
    hilo->detached = 1;
    return 0;
}

/**
 * encolar_mutex
 *
 * Añade el TCB de un hilo a la cola de espera de un mutex. Si la cola estaba
 * vacía, este hilo pasa a ser cabeza y cola, si no estaba vacia, se encola al final
 * (despues de tail, y pasa a ser el nuevo tail).
 *
 * Entradas:
 *  - mutex: puntero al mutex.
 *  - hilo : puntero al TCB del hilo que se va a encolar.
 *
 * Retorna:
 *  - Ninguna
 */
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


/**
 * desencolar_mutex
 *
 * Extrae el primer hilo en espera de la cola del mutex. Si la cola está vacía,
 * retorna NULL. Si se extrae un hilo, actualiza head, limpia next del hilo y lo
 * retorna.
 *
 * Entradas:
 *  - mutex: puntero al mutex.
 *
 * Retorna:
 *  - TCB*: puntero al TCB extraído, o NULL si no había hilos.
 */
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

/**
 * my_mutex_init
 *
 * Inicializa un mutex con valores NULL o 0. Si el puntero recibido es NULL,
 * retorna -1.
 *
 * Entradas:
 *  - mutex: puntero al mutex que se va a inicializar.
 *
 * Retorna:
 *  - 0 si la inicialización fue correcta, -1 si mutex es NULL.
 */
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

/**
 * my_mutex_destroy
 *
 * Destruye un mutex que no esté bloqueado y no tenga hilos en espera. Si
 * mutex es NULL, está bloqueado o aún tiene una lista de espera, retorna -1. En
 * caso contrario,reestablece los valores a 0 o Null.
 *
 * Entradas:
 *  - mutex: puntero al mutex que se va a destruir.
 *
 * Retorna:
 *  - 0 si la destrucción fue exitosa, -1 si hay algun error.
 */
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

/**
 * my_mutex_lock
 *
 * Intenta apropiarse del mutex. Si mutex es NULL, retorna -1. Si el mutex está libre,
 * lo marca como bloqueado y establece propietario = hilo_actual, retornando 0. Si el
 * mutex ya pertenece al hilo actual, retorna -1. Si está
 * bloqueado por otro hilo, encola hilo_actual en la cola de espera, marca su estado
 * como BLOCKED y llama a schedule().
 *
 * Entradas:
 *  - mutex: puntero al mutex que se desea bloquear.
 *
 * Retorna:
 *  - 0 si logra bloquear, -1 si mutex es NULL o lo solicita el mismo hilo propietario.
 */
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

/**
 * my_mutex_trylock
 *
 * Intenta adquirir el mutex, pero si ya está bloqueado, no se encola ni se bloquea.
 * Si mutex es NULL, retorna -1. Si el mutex está libre, lo marca como bloqueado,
 * asigna propietario = hilo_actual y devuelve 0. Si ya está ocupado, retorna -1
 * sin encolarse ni cambiar el estado a BLOCKED.
 *
 * Entradas:
 *  - mutex: puntero al mutex que se va a intentar bloquear.
 *
 * Retorna:
 *  - 0 si lo adquirió, -1 si mutex era NULL o ya estaba bloqueado.
 */
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

/**
 * my_mutex_unlock
 *
 * Libera el mutex que posee el hilo actual. Si mutex es NULL, no estaba bloqueado o
 * propietario != hilo_actual, retorna -1. Si hay hilos en espera, desencola el
 * siguiente, lo marca como READY, lo encola en su scheduler y le asigna el mutex como
 * nuevo propietario. Si no hay ningún hilo en la cola, simplemente libera el mutex.
 *
 * Entradas:
 *  - mutex: puntero al mutex que se va a liberar.
 *
 * Retorna:
 *  - 0 si la operación tuvo éxito, -1 si hubo error.
 */
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