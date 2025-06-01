#define _POSIX_C_SOURCE 200809L

#include "../include/scheduler.h"
#include <stdlib.h>     // malloc, free, realloc
#include <signal.h>     // sigaction
#include <stdio.h>
#include <time.h>
#include <sys/time.h>   // setitimer, struct itimerval
#include <string.h>


#define STACK_SIZE  (1024 * 64)  // Tamaño de pila: 64 KB
#define QUANTUM_MS   100         // Quantum de 100 milisegundos
#define MAX_SNAPSHOTS 10000
static char *rr_snapshots[MAX_SNAPSHOTS];
static int   rr_snapshot_count = 0;
int scheduler_activo = 0;

ThreadPool   global_thread_pool = { 0, NULL, 0, 0 };
TCB         *hilo_actual        = NULL;
int          next_tid           = 0;
ucontext_t   scheduler_ctx;




/**
 * threadpool_alive_count
 *
 * Recorre el conjunto global de hilos (global_thread_pool) y cuenta
 * cuántos de ellos aún no han alcanzado el estado TERMINATED.
 *
 * Entradas:
 *   ninguna
 *
 * Retorna:
 *   int – número de hilos en el pool cuyo estado es distinto de TERMINATED.
 */
int threadpool_alive_count(void) {
    int cnt = 0;
    for (size_t i = 0; i < global_thread_pool.count; ++i) {
        TCB *t = global_thread_pool.threads[i];
        if (t && t->state != TERMINATED) {
            ++cnt;
        }
    }
    return cnt;
}



/**
 * ensure_capacity
 *
 * Verifica si el pool de hilos tiene espacio para un nuevo TCB; de no ser así,
 * duplica (o inicializa) la capacidad y realoca el arreglo de hilos.
 *
 * Entradas:
 *   ThreadPool *pool – puntero a la estructura del pool de hilos.
 *
 * Retorna:
 *   void – no retorna ningún valor, pero ajusta pool->threads y pool->capacity
 *   si es necesario.
 */
static void ensure_capacity(ThreadPool *pool) {
    if (pool->count + 1 > pool->capacity) {
        size_t new_cap = (pool->capacity == 0 ? 4 : pool->capacity * 2);
        pool->threads = realloc(pool->threads, new_cap * sizeof(TCB *));
        pool->capacity = new_cap;
    }
}


/**
 * registrar_hilo
 *
 * Agrega un nuevo hilo al pool, asegurando capacidad y actualizando el conteo.
 *
 * Entradas:
 *   ThreadPool *pool – puntero al pool de hilos.
 *   TCB *hilo – puntero al bloque de control del hilo a registrar.
 *
 * Retorna:
 *   int – el tid del hilo registrado.
 */
int registrar_hilo(ThreadPool *pool, TCB *hilo) {
    ensure_capacity(pool);
    pool->threads[pool->count++] = hilo;
    return hilo->tid;
}


/**
 * buscar_hilo_id
 *
 * Busca en el pool un hilo cuyo tid coincida con el proporcionado.
 *
 * Entradas:
 *   ThreadPool *pool – puntero al pool de hilos.
 *   int tid – identificador del hilo a buscar.
 *
 * Retorna:
 *   TCB* – puntero al TCB del hilo encontrado o NULL si no existe.
 */
TCB *buscar_hilo_id(ThreadPool *pool, int tid) {
    for (size_t i = 0; i < pool->count; i++) {
        if (pool->threads[i]->tid == tid) {
            return pool->threads[i];
        }
    }
    return NULL;
}

/**
 * encolar_hilo
 *
 * Llama a la función específica del scheduler para encolar un hilo en la estructura interna.
 *
 * Entradas:
 *   Scheduler *sched – puntero al scheduler que provee la función encolar_hilo.
 *   TCB *hilo – puntero al TCB del hilo a encolar.
 *
 * Retorna:
 *   void – no retorna valor, da la operación al metodo interno del scheduler.
 */
void encolar_hilo(Scheduler *sched, TCB *hilo) {
    sched->encolar_hilo(sched, hilo);
}


/**
 * my_thread_chsched
 *
 * Cambia el scheduler asignado a un hilo, removiéndolo del scheduler actual
 * y encolándolo en el nuevo scheduler. Además, actualiza el estado del hilo a READY.
 *
 * Entradas:
 *   TCB *hilo – puntero al TCB del hilo cuyo scheduler se cambia.
 *   Scheduler *new_sch – puntero al nuevo scheduler donde se debe encolar el hilo.
 *
 * Retorna:
 *   int – 0 si la operación se realizó correctamente.
 */
int my_thread_chsched(TCB *hilo, Scheduler *new_sch) {

    Scheduler *old_sch = hilo->scheduler;
    if (old_sch)
        old_sch->remover_hilo(old_sch, hilo);
    hilo->scheduler = new_sch;
    hilo->state     = READY;
    hilo->next      = NULL;
    new_sch->encolar_hilo(new_sch, hilo);

    return 0;
}



/**
 * schedule
 *
 * Si existe un hilo actual, solicita al scheduler asociado el siguiente hilo listo
 * para ejecutarse, en caso de que haya uno, intercambia el contexto entre el hilo
 * actual y el siguiente, permitiendo la ejecución del nuevo hilo.
 *
 * Entradas:
 *   ninguna
 *
 * Retorna:
 *   void – no retorna valor, cambia el hilo en ejecución mediante swapcontext.
 */
void schedule(void) {
    if (hilo_actual == NULL) {
        return;
    }
    TCB *prev      = hilo_actual;
    Scheduler *sch = prev->scheduler;
    TCB *next      = sch->siguiente_hilo(sch);

    if (next == NULL) {

        return;
    }
    hilo_actual = next;
    swapcontext(&prev->context, &next->context);

}

/**
 * alarm_handler
 *
 * Captura la señal de alarma (SIGALRM) y fuerza la llamada a schedule()
 * para cambiar al siguiente hilo listo para ejecutar.
 *
 * Entradas:
 *   int sig – número de señal recibida (por lo general SIGALRM).
 *
 * Retorna:
 *   void – no retorna valor, invoca schedule() internamente.
 */
static void alarm_handler(int sig) {
    (void)sig;
    schedule();
}


/**
 * start_preemption
 *
 * Configura un manejador de señales para SIGALRM y programa un temporizador
 * que genere señales periódicas cada quantum_ms milisegundos, forzando la
 * invocación de schedule() para preempción de hilos.
 *
 * Entradas:
 *   int quantum_ms – duración del quantum en milisegundos.
 *
 * Retorna:
 *   void – no retorna valor, inicializa el temporizador de preempción.
 */
static void start_preemption(int quantum_ms) {
    struct sigaction sa;
    sigemptyset(&sa.sa_mask);
    sa.sa_handler = alarm_handler;
    sa.sa_flags   = 0;
    sigaction(SIGALRM, &sa, NULL);


    struct itimerval timer = {
        .it_interval = { .tv_sec = quantum_ms/1000,
                         .tv_usec = (quantum_ms%1000)*1000 },
        .it_value    = { .tv_sec = quantum_ms/1000,
                         .tv_usec = (quantum_ms%1000)*1000 }
    };
    setitimer(ITIMER_REAL, &timer, NULL);
}



/**
 * rr_encolar_hilo
 *
 * Encola un hilo en la cola del scheduler Round Robin, actualizando los punteros
 * head y tail y estableciendo el estado READY.
 *
 * Entradas:
 *   Scheduler *sched – puntero al scheduler RR donde se encola el hilo.
 *   TCB *hilo – puntero al TCB del hilo a encolar.
 *
 * Retorna:
 *   void – no retorna valor, modifica las estructuras internas del scheduler.
 */
static void rr_encolar_hilo(Scheduler *sched, TCB *hilo) {

    RR_Scheduler *rr = (RR_Scheduler*)sched;

    hilo->scheduler     = sched;
    hilo->state         = READY;
    hilo->next          = NULL;
    if (rr->tail == NULL) {
        rr->head = hilo;
    }
    else {
        rr->tail->next = hilo;
    }
    rr->tail = hilo;


}

/**
 * rr_siguiente_hilo
 *
 * Obtiene el siguiente hilo listo para ejecutar en el scheduler Round Robin.
 * - Elimina de la cola los hilos cuyo estado es TERMINATED.
 * - Si la cola está vacía, retorna NULL.
 * - Extrae el hilo en la cabeza de la cola.
 * - Si su estado es READY, lo reencola al final para futura ejecución.
 *
 * Entradas:
 *   Scheduler *sched – puntero al scheduler RR.
 *
 * Retorna:
 *   TCB* – puntero al TCB del hilo seleccionado o NULL si no hay hilos listos.
 */
static TCB *rr_siguiente_hilo(Scheduler *sched) {
    RR_Scheduler *rr = (RR_Scheduler*)sched;

    while (rr->head && rr->head->state == TERMINATED) {
        TCB *dead = rr->head;
        rr->head = dead->next;
        if (dead == rr->tail) {
            rr->tail = NULL;
        }
        dead->next = NULL;
    }

    if (!rr->head) {
        return NULL;
    }

    TCB *chosen = rr->head;
    rr->head    = chosen->next;
    if (!rr->head) {
        rr->tail = NULL;
    }
    chosen->next = NULL;

    if (chosen->state == READY) {
        if (rr->tail) {
            rr->tail->next = chosen;
        } else {
            rr->head = chosen;
        }
        rr->tail = chosen;
    }

    return chosen;
}

/**
 * rr_remover_hilo
 *
 * Elimina un hilo específico de la cola del scheduler Round Robin, ajustando
 * los punteros head y tail según corresponda.
 *
 * Entradas:
 *   Scheduler *sched – puntero al scheduler RR del cual se debe remover el hilo.
 *   TCB *hilo – puntero al TCB del hilo que se desea eliminar.
 *
 * Retorna:
 *   void – no retorna valor, modifica la estructura interna del scheduler eliminando el hilo.
 */

static void rr_remover_hilo(Scheduler *sched, TCB *hilo) {
    RR_Scheduler *rr = (RR_Scheduler*)sched;
    TCB *prev = NULL;
    TCB *it = rr->head;

    while (it && it != hilo) {
        prev = it;
        it   = it->next;
    }
    if (!it) {
        return;
    }

    if (prev) {
        prev->next = it->next;
    }
    else {
        rr->head = it->next;
    }
    if (rr->tail == it) {
        rr->tail = prev;
    }
    it->next = NULL;
}


/**
 * rr_scheduler_init
 *
 * Inicializa el scheduler Round Robin, asignando los punteros a las
 * funciones de encolado, selección y remover de hilos; establece el
 * quantum de tiempo, limpia la cabeza y cola, activa el scheduler y
 * arranca el temporizador de preempción.
 *
 * Entradas:
 *   RR_Scheduler *rr – puntero al struct RR_Scheduler a inicializar.
 *   int quantum_ms – duración del quantum en milisegundos.
 *
 * Retorna:
 *   void – no retorna valor, configura la estructura y arranca la preempción.
 */
void rr_scheduler_init(RR_Scheduler *rr, int quantum_ms) {
    rr->base.encolar_hilo   = rr_encolar_hilo;
    rr->base.siguiente_hilo = rr_siguiente_hilo;
    rr->base.remover_hilo    = rr_remover_hilo;
    rr->quantum             = quantum_ms;
    rr->head = rr->tail     = NULL;
    scheduler_activo = 1;
    start_preemption(quantum_ms);


}




//--------------------------------------------------------------
//Lottery Scheduler
//--------------------------------------------------------------


/**
 * lottery_encolar_hilo
 *
 * Encola un hilo en la lista del scheduler Lottery, asignándole su scheduler,
 * marcándolo como READY y agregándolo al final de la lista enlazada.
 *
 * Entradas:
 *   Scheduler *sched – puntero al scheduler de tipo Lottery.
 *   TCB *hilo – puntero al bloque de control del hilo a encolar.
 *
 * Retorna:
 *   void – no retorna valor, modifica la estructura interna del scheduler.
 */
static void lottery_encolar_hilo(Scheduler *sched, TCB *hilo) {
    Lottery_Scheduler *ls = (Lottery_Scheduler*)sched;
    hilo->scheduler = sched;
    hilo->state     = READY;
    hilo->next      = NULL;

    if (ls->head == NULL) {
        ls->head = hilo;
    } else {
        TCB *it = ls->head;
        while (it->next) {
            it = it->next;
        }
        it->next = hilo;
    }
}

/**
 * lottery_siguiente_hilo
 *
 * Selecciona el siguiente hilo a ejecutar en el scheduler Lottery:
 * - Si existe un hilo actualmente en RUNNING, lo cambia a READY y lo reencola.
 * - Calcula el total de boletos de todos los hilos en READY.
 * - Genera un número aleatorio entre 1 y total, y encuentra el hilo ganador
 *   acumulando boletos hasta alcanzar el valor aleatorio.
 * - Remueve al hilo ganador de la lista y lo marca como RUNNING.
 *
 * Entradas:
 *   Scheduler *sched – puntero al scheduler de tipo Lottery.
 *
 * Retorna:
 *   TCB* – puntero al TCB del hilo ganador (estado RUNNING), o NULL si no hay
 *          hilos listos o si la suma de boletos es menor igual a 0.
 */
static TCB *lottery_siguiente_hilo(Scheduler *sched) {
    Lottery_Scheduler *ls = (Lottery_Scheduler*)sched;
    TCB *prev = hilo_actual;

    if (prev && prev->state == RUNNING) {
        prev->state = READY;
        prev->next  = NULL;

        if (!ls->head) {
            ls->head = prev;
        } else {
            TCB *it = ls->head;
            while (it->next) it = it->next;
            it->next = prev;
        }
    }

    int total = 0;
    for (TCB *it = ls->head; it; it = it->next) {
        if (it->state == READY)
            total += it->tickets;
    }
    if (total <= 0)
        return NULL;

    int winner = (rand() % total) + 1;
    int acc    = 0;

    TCB *mejor = NULL;
    TCB *prev_mejor = NULL;
    TCB *it = ls->head;
    TCB *prev_it = NULL;

    for (; it; prev_it = it, it = it->next) {
        if (it->state != READY)
            continue;
        acc += it->tickets;
        if (acc >= winner) {
            mejor       = it;
            prev_mejor  = prev_it;
            break;
        }
    }

    if (!mejor)
        return NULL;

    if (prev_mejor)
        prev_mejor->next = mejor->next;
    else
        ls->head = mejor->next;

    mejor->next = NULL;
    mejor->state = RUNNING;
    return mejor;
}

/**
 * lottery_remover_hilo
 *
 * Elimina un hilo específico de la lista del scheduler Lottery, ajustando
 * punteros para removerlo de la lista enlazada.
 *
 * Entradas:
 *   Scheduler *sched – puntero al scheduler de tipo Lottery del cual se remueve el hilo.
 *   TCB *hilo – puntero al bloque de control del hilo que se desea eliminar.
 *
 * Retorna:
 *   void – no retorna valor, modifica la estructura interna del scheduler eliminando el hilo.
 */

static void lottery_remover_hilo(Scheduler *sched, TCB *hilo) {
    Lottery_Scheduler *ls = (Lottery_Scheduler*)sched;
    TCB *prev = NULL;
    TCB *it = ls->head;
    while (it && it != hilo) {
        prev = it;
        it   = it->next;
    }
    if (!it) {
        return;
    }
    if (prev) {
        prev->next = it->next;
    }
    else {
        ls->head  = it->next;
    }
    it->next = NULL;
}


/**
 * lottery_scheduler_init
 *
 * Inicializa el scheduler Lottery, asignando las funciones de encolado, selección y remover de hilos;
 * establece la cabeza de la lista en NULL, configura el quantum de tiempo, activa el scheduler,
 * arranca el temporizador de preempción y siembra el generador de números aleatorios.
 *
 * Entradas:
 *   Lottery_Scheduler *ls – puntero al struct Lottery_Scheduler a inicializar.
 *   int quantum_ms – duración del quantum en milisegundos.
 *
 * Retorna:
 *   void – no retorna valor, configura la estructura interna y arranca la preempción.
 */
void lottery_scheduler_init(Lottery_Scheduler *ls, int quantum_ms) {
    ls->base.encolar_hilo   = lottery_encolar_hilo;
    ls->base.siguiente_hilo = lottery_siguiente_hilo;
    ls->base.remover_hilo    = lottery_remover_hilo;
    ls->head                = NULL;
    ls->quantum             = quantum_ms;
    scheduler_activo = 2;
    start_preemption(quantum_ms);
    srand((unsigned)time(NULL));
}



//--------------------------------------------------------------
//Real Time Scheduler con EDF
//--------------------------------------------------------------


/**
 * edf_siguiente_hilo
 *
 * Recorre la lista de hilos del scheduler EDF y selecciona el hilo READY con
 * el deadline más cercano. Marca ese hilo como RUNNING antes de retornarlo.
 *
 * Entradas:
 *   Scheduler *sched – puntero al scheduler de tipo EDF.
 *
 * Retorna:
 *   TCB* – puntero al TCB del hilo seleccionado (con el menor deadline) marcado como RUNNING,
 *          o NULL si no hay hilos en READY.
 */
static TCB *edf_siguiente_hilo(Scheduler *sched) {
    EDF_Scheduler *edf_scheduler = (EDF_Scheduler*)sched;
    TCB *mejor = NULL;
    for (TCB *it = edf_scheduler->head; it; it = it->next) {

        if (it->state != READY)
            continue;

        if (!mejor || it->deadline < mejor->deadline)
            mejor = it;
    }
    if (!mejor)
        return NULL;
    mejor->state = RUNNING;
    return mejor;
}


/**
 * edf_encolar_hilo
 *
 * Agrega un hilo a la lista del scheduler EDF, marcándolo como READY.
 * Si el nuevo hilo tiene un deadline menor al del hilo actualmente en ejecución,
 * se fuerza un cambio de contexto para ejecutar de inmediato el hilo con deadline más cercano.
 *
 * Entradas:
 *   Scheduler *sched – puntero al scheduler de tipo EDF.
 *   TCB *hilo – puntero al TCB del hilo a encolar.
 *
 * Retorna:
 *   void – no retorna valor, modifica la estructura interna del scheduler y
 *          puede invocar swapcontext si el nuevo hilo tiene
 *          deadline más cercano que el hilo actual.
 */
static void edf_encolar_hilo(Scheduler *sched, TCB *hilo) {

    EDF_Scheduler *edf_scheduler = (EDF_Scheduler*)sched;
    hilo->scheduler = sched;
    hilo->state     = READY;
    hilo->next      = NULL;
    if (!edf_scheduler->head) {
        edf_scheduler->head = hilo;
    } else {
        TCB *it = edf_scheduler->head;
        while (it->next)
            it = it->next;
        it->next = hilo;
    }
    if (hilo_actual && (hilo->deadline < hilo_actual->deadline)) {
        TCB *prev = hilo_actual;
        prev->state = READY;
        TCB *next = edf_siguiente_hilo(sched);
        if (next && next != prev) {
            next->state = RUNNING;
            hilo_actual = next;
            swapcontext(&prev->context, &next->context);
        }
    }

}


/**
 * edf_remover_hilo
 *
 * Elimina un hilo específico de la lista del scheduler EDF, ajustando punteros
 * para removerlo de la lista enlazada.
 *
 * Entradas:
 *   Scheduler *sched – puntero al scheduler de tipo EDF del cual se remueve el hilo.
 *   TCB *hilo – puntero al bloque de control del hilo que se desea eliminar.
 *
 * Retorna:
 *   void – no retorna valor, modifica la estructura interna del scheduler eliminando el hilo.
 */
static void edf_remover_hilo(Scheduler *sched, TCB *hilo) {
    EDF_Scheduler *edf_scheduler = (EDF_Scheduler*)sched;
    TCB *prev = NULL;
    TCB *it = edf_scheduler->head;
    while (it && it != hilo) {
        prev = it;
        it   = it->next;
    }
    if (!it) {
        return;
    }
    if (prev) {
        prev->next = it->next;
    }
    else {
        edf_scheduler->head  = it->next;
    }
    it->next = NULL;
}


/**
 * edf_scheduler_init
 *
 * Inicializa el scheduler EDF, asignando las funciones de encolado, selección y remover de hilos;
 * establece la cabeza de la lista en NULL y desactiva la preempción específica de EDF.
 *
 * Entradas:
 *   EDF_Scheduler *edf_scheduler – puntero al struct EDF_Scheduler a inicializar.
 *
 * Retorna:
 *   void – no retorna valor; configura la estructura interna del scheduler.
 */
void edf_scheduler_init(EDF_Scheduler *edf_scheduler) {
    edf_scheduler->base.encolar_hilo   = edf_encolar_hilo;
    edf_scheduler->base.siguiente_hilo = edf_siguiente_hilo;
    edf_scheduler->base.remover_hilo    = edf_remover_hilo;
    edf_scheduler->head                = NULL;
    scheduler_activo = 0;
}
