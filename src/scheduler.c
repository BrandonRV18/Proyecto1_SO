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




static void ensure_capacity(ThreadPool *pool) {
    if (pool->count + 1 > pool->capacity) {
        size_t new_cap = (pool->capacity == 0 ? 4 : pool->capacity * 2);
        pool->threads = realloc(pool->threads, new_cap * sizeof(TCB *));
        pool->capacity = new_cap;
    }
}

int registrar_hilo(ThreadPool *pool, TCB *hilo) {
    ensure_capacity(pool);
    pool->threads[pool->count++] = hilo;
    return hilo->tid;
}

TCB *buscar_hilo_id(ThreadPool *pool, int tid) {
    for (size_t i = 0; i < pool->count; i++) {
        if (pool->threads[i]->tid == tid) {
            return pool->threads[i];
        }
    }
    return NULL;
}


void encolar_hilo(Scheduler *sched, TCB *hilo) {
    sched->encolar_hilo(sched, hilo);
}

//Cambia dinámicamente la política de scheduling de un hilo
int my_thread_chsched(TCB *hilo, Scheduler *new_sch) {
    //Obtiene el scheduler actual al que está asignado el hilo
    Scheduler *old_sch = hilo->scheduler;

    //Si ya tenía un scheduler, lo saca de su lista de listos
    if (old_sch)
        old_sch->remover_hilo(old_sch, hilo);

    //Asigna el nuevo scheduler al hilo
    hilo->scheduler = new_sch;

    //Marca el hilo como READY para que pueda ser seleccionado
    hilo->state     = READY;

    //Limpia cualquier enlace previo en la lista antigua
    hilo->next      = NULL;

    //Encola el hilo en la nueva política de scheduling
    new_sch->encolar_hilo(new_sch, hilo);

    return 0;
}




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


static void alarm_handler(int sig) {
    (void)sig;
    schedule();
}

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


static TCB *rr_siguiente_hilo(Scheduler *sched) {
    RR_Scheduler *rr = (RR_Scheduler*)sched;

    // 1) Limpia del frente todos los hilos TERMINATED
    while (rr->head && rr->head->state == TERMINATED) {
        TCB *dead = rr->head;
        rr->head = dead->next;
        if (dead == rr->tail) {
            rr->tail = NULL;
        }
        dead->next = NULL;
    }

    // 2) Si ya no queda nadie, devolvemos NULL
    if (!rr->head) {
        return NULL;
    }

    // 3) Sacamos el primero de la lista...
    TCB *chosen = rr->head;
    rr->head    = chosen->next;
    if (!rr->head) {
        rr->tail = NULL;
    }
    chosen->next = NULL;

    // 4) Si sigue READY (no terminó), lo reencolamos al final
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

// Quita t de la cola FIFO de RR
static void rr_remover_hilo(Scheduler *sched, TCB *hilo) {
    RR_Scheduler *rr = (RR_Scheduler*)sched;
    TCB *prev = NULL;
    TCB *it = rr->head;
    // Busca t en la lista
    while (it && it != hilo) {
        prev = it;
        it   = it->next;
    }
    if (!it) {
        return;
    }
    // Desenlaza
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

void rr_scheduler_init(RR_Scheduler *rr, int quantum_ms) {
    rr->base.encolar_hilo   = rr_encolar_hilo;
    rr->base.siguiente_hilo = rr_siguiente_hilo;
    rr->base.remover_hilo    = rr_remover_hilo;
    rr->quantum             = quantum_ms;
    rr->head = rr->tail     = NULL;
    scheduler_activo = 1;
    start_preemption(quantum_ms);


}


// -------------------------------------------------------------
// thread_func()
// Función de prueba que ejecuta cada hilo
// -------------------------------------------------------------
void thread_func(int id) {
    for (int i = 0; i < 5; i++) {
        printf("Hilo %d, iteración %d\n", id, i);
        // Bucle pesado para que se note la preempción
        for (volatile int j = 0; j < 100000000; j++) {
            // Simulación de carga de CPU
        }
    }
    // Al terminar, marcamos TERMINATED y cedemos CPU
    hilo_actual->state = TERMINATED;
    schedule();
}

//--------------------------------------------------------------
//Lottery Scheduler
//--------------------------------------------------------------

// Inserta un hilo al final de la lista READY
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

// Quita t de la lista enlazada de LOTTERY
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


// Inicializa el scheduler de lotería (sin preempción periódica)
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

/* ------------------------------------------------------------------ */
/* Selecciona el READY con deadline más cercano*/
/* ------------------------------------------------------------------ */
static TCB *edf_siguiente_hilo(Scheduler *sched) {
    /* Down-cast al tipo concreto */
    EDF_Scheduler *edf_scheduler = (EDF_Scheduler*)sched;
    /* Puntero al mejor candidato */
    TCB *mejor = NULL;

    /* Recorre todos los hilos listos */
    for (TCB *it = edf_scheduler->head; it; it = it->next) {
        /* Solo considera los que están READY */
        if (it->state != READY)
            continue;
        /* Actualiza si es el primero o tiene deadline más temprano */
        if (!mejor || it->deadline < mejor->deadline)
            mejor = it;
    }

    /* Si no encontró ninguno, devuelve NULL */
    if (!mejor)
        return NULL;

    /* Marca el ganador como en ejecución */
    mejor->state = RUNNING;
    /* Devuelve el TCB seleccionado */

    return mejor;
}
/* ------------------------------------------------------------------ */
/* Inserta un hilo en READY y pregunta si su deadline es más temprano */
/* ------------------------------------------------------------------ */
static void edf_encolar_hilo(Scheduler *sched, TCB *hilo) {

    /* Down-cast al tipo concreto */
    EDF_Scheduler *edf_scheduler = (EDF_Scheduler*)sched;
    /* Asocia este scheduler al hilo */
    hilo->scheduler = sched;
    /* Marca el hilo como listo */
    hilo->state     = READY;
    /* Rompe enlaces previos */
    hilo->next      = NULL;


    /* Inserta al final de la lista enlazada */
    if (!edf_scheduler->head) {
        edf_scheduler->head = hilo;
    } else {
        TCB *it = edf_scheduler->head;
        while (it->next)
            it = it->next;
        it->next = hilo;
    }

    /* Si hay un hilo en ejecución y este nuevo tiene
       deadline más temprano, preempta inmediatamente */


    if (hilo_actual && (hilo->deadline < hilo_actual->deadline)) {

        /* Cede el control al scheduler */
        TCB *prev = hilo_actual;

        // no re-encolamos otra vez al current, ya está en la lista
        // marcamos al current como listo
        prev->state = READY;

        // seleccionamos al más urgente
        TCB *next = edf_siguiente_hilo(sched);
        if (next && next != prev) {
            next->state = RUNNING;
            hilo_actual = next;
            swapcontext(&prev->context, &next->context);
        }
    }

}


// Idéntico a lottery_remove_hilo, pues usa lista enlazada
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



void edf_scheduler_init(EDF_Scheduler *edf_scheduler) {
    edf_scheduler->base.encolar_hilo   = edf_encolar_hilo;
    edf_scheduler->base.siguiente_hilo = edf_siguiente_hilo;
    edf_scheduler->base.remover_hilo    = edf_remover_hilo;
    edf_scheduler->head                = NULL;
    scheduler_activo = 0;
}

static TCB hilo_nuevo;
static EDF_Scheduler *ES;
/* Función de cada hilo: imprime iteraciones, y uno crea el nuevo hilo */
static void thread_func_edf(int id) {
    for (int i = 0; i < 5; i++) {
        printf("Hilo %d, iteración %d\n", id, i);
        /* Simular carga */
        for (volatile int j = 0; j < 50000000; j++);
        /* En el hilo 1, en la iteración 2, nace un hilo 4 más urgente */
        if (id == 1 && i == 2) {
            /* Configurar el TCB estático */
            getcontext(&hilo_nuevo.context);
            hilo_nuevo.stack = malloc(64*1024);
            hilo_nuevo.context.uc_stack.ss_sp   = hilo_nuevo.stack;
            hilo_nuevo.context.uc_stack.ss_size = 64*1024;
            hilo_nuevo.context.uc_link          = NULL;
            makecontext(&hilo_nuevo.context, (void(*)(void))thread_func, 1, 4);
            hilo_nuevo.tid      = 4;
            hilo_nuevo.deadline = 50;  /* Muy urgente: 50ms */
            hilo_nuevo.next     = NULL;
            /* Encolar en EDF y preemptar al vuelo */
            ES->base.encolar_hilo((Scheduler*)ES, &hilo_nuevo);
        }
    }
    /* Al terminar, liberar pila y ceder */
    int id_local = hilo_actual->tid;
    free(hilo_actual->stack);
    hilo_actual->state = TERMINATED;
    schedule();
}

// -------------------------------------------------------------
// main()
// Crea tres hilos de ejemplo y arranca el scheduler Round Robin
// -------------------------------------------------------------
/*
int main(void) {
    // Crear e inicializar el scheduler Round Robin
    RR_Scheduler rr;
    rr_scheduler_init(&rr, QUANTUM_MS);

    TCB t1, t2, t3;
    int arg1 = 1, arg2 = 2, arg3 = 3;

    // Configurar contexto y pila de t1
    getcontext(&t1.context);
    t1.stack = malloc(STACK_SIZE);
    t1.context.uc_stack.ss_sp   = t1.stack;
    t1.context.uc_stack.ss_size = STACK_SIZE;
    t1.context.uc_link          = NULL;
    makecontext(&t1.context, (void(*)(void))thread_func, 1, arg1);
    t1.tid   = 1;
    t1.state = READY;

    // Configurar contexto y pila de t2
    getcontext(&t2.context);
    t2.stack = malloc(STACK_SIZE);
    t2.context.uc_stack.ss_sp   = t2.stack;
    t2.context.uc_stack.ss_size = STACK_SIZE;
    t2.context.uc_link          = NULL;
    makecontext(&t2.context, (void(*)(void))thread_func, 1, arg2);
    t2.tid   = 2;
    t2.state = READY;

    //Configurar contexto y pila de t3
    getcontext(&t3.context);
    t3.stack = malloc(STACK_SIZE);
    t3.context.uc_stack.ss_sp   = t3.stack;
    t3.context.uc_stack.ss_size = STACK_SIZE;
    t3.context.uc_link          = NULL;
    makecontext(&t3.context, (void(*)(void))thread_func, 1, arg3);
    t3.tid   = 3;
    t3.state = READY;

    //Encolar los hilos en la cola de listos de RR
    rr.base.encolar_hilo((Scheduler *)&rr, &t1);
    rr.base.encolar_hilo((Scheduler *)&rr, &t2);
    rr.base.encolar_hilo((Scheduler *)&rr, &t3);

    // Arrancar la ejecución con el primer hilo (t1)
    hilo_actual = &t1;
    swapcontext(&(ucontext_t){0}, &t1.context);

    // Al volver al main, liberar la memoria de las pilas
    free(t1.stack);
    free(t2.stack);
    free(t3.stack);
    return 0;
}
*/

// -------------------------------------------------------------
// main(): prueba completa del Lottery Scheduler con hilos
// -------------------------------------------------------------
/*
int main(void) {
    Lottery_Scheduler ls;                             // 68) declarar scheduler
    lottery_scheduler_init(&ls, 100);                      // 69) inicializarlo

    // 70) crear tres TCB de prueba
    TCB t1, t2, t3;
    int arg1 = 1, arg2 = 2, arg3 = 3;

    // 71) configurar contexto y pila de t1
    getcontext(&t1.context);
    t1.stack = malloc(STACK_SIZE);
    t1.context.uc_stack.ss_sp   = t1.stack;
    t1.context.uc_stack.ss_size = STACK_SIZE;
    t1.context.uc_link          = NULL;
    makecontext(&t1.context, (void(*)(void))thread_func, 1, arg1);
    t1.tid     = 1;                                   // 72) ID
    t1.tickets = 10;                                  // 73) tickets
    t1.state   = READY;                               // 74) estado
    t1.next    = NULL;                                // 75) next limpio

    // 76) t2 idéntico con sus valores
    getcontext(&t2.context);
    t2.stack = malloc(STACK_SIZE);
    t2.context.uc_stack.ss_sp   = t2.stack;
    t2.context.uc_stack.ss_size = STACK_SIZE;
    t2.context.uc_link          = NULL;
    makecontext(&t2.context, (void(*)(void))thread_func, 1, arg2);
    t2.tid     = 2;
    t2.tickets = 20;
    t2.state   = READY;
    t2.next    = NULL;

    // 77) t3 idem
    getcontext(&t3.context);
    t3.stack = malloc(STACK_SIZE);
    t3.context.uc_stack.ss_sp   = t3.stack;
    t3.context.uc_stack.ss_size = STACK_SIZE;
    t3.context.uc_link          = NULL;
    makecontext(&t3.context, (void(*)(void))thread_func, 1, arg3);
    t3.tid     = 3;
    t3.tickets = 30;
    t3.state   = READY;
    t3.next    = NULL;

    // 78) encolamos los tres hilos
    ls.base.encolar_hilo((Scheduler*)&ls, &t1);
    ls.base.encolar_hilo((Scheduler*)&ls, &t2);
    ls.base.encolar_hilo((Scheduler*)&ls, &t3);

    // 79) arrancamos con el primer ganador
    hilo_actual = ls.base.siguiente_hilo((Scheduler*)&ls);
    swapcontext(&(ucontext_t){0}, &hilo_actual->context);

    // 80) al volver, liberamos pilas
    free(t1.stack);
    free(t2.stack);
    free(t3.stack);
    return 0;
}
*/
/* main de prueba edf */
/*
int main(void) {
    EDF_Scheduler edf;
    edf_scheduler_init(&edf);
    ES = &edf;


    TCB t1, t2, t3;
    int a1 = 1, a2 = 2, a3 = 3;


    getcontext(&t1.context);
    t1.stack = malloc(64*1024);
    t1.context.uc_stack.ss_sp   = t1.stack;
    t1.context.uc_stack.ss_size = 64*1024;
    t1.context.uc_link          = NULL;
    makecontext(&t1.context, (void(*)(void))thread_func_edf, 1, a1);
    t1.tid      = 1;
    t1.deadline = 200;
    t1.state    = READY;
    t1.next     = NULL;


    getcontext(&t2.context);
    t2.stack = malloc(64*1024);
    t2.context.uc_stack.ss_sp   = t2.stack;
    t2.context.uc_stack.ss_size = 64*1024;
    t2.context.uc_link          = NULL;
    makecontext(&t2.context, (void(*)(void))thread_func, 1, a2);
    t2.tid      = 2;
    t2.deadline = 150;
    t2.state    = READY;
    t2.next     = NULL;


    getcontext(&t3.context);
    t3.stack = malloc(64*1024);
    t3.context.uc_stack.ss_sp   = t3.stack;
    t3.context.uc_stack.ss_size = 64*1024;
    t3.context.uc_link          = NULL;
    makecontext(&t3.context, (void(*)(void))thread_func, 1, a3);
    t3.tid      = 3;
    t3.deadline = 300;
    t3.state    = READY;
    t3.next     = NULL;


    edf.base.encolar_hilo((Scheduler*)&edf, &t1);
    edf.base.encolar_hilo((Scheduler*)&edf, &t2);
    edf.base.encolar_hilo((Scheduler*)&edf, &t3);


    hilo_actual = edf.base.siguiente_hilo((Scheduler*)&edf);
    swapcontext(&(ucontext_t){0}, &hilo_actual->context);

    return 0;
}
*/