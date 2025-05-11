#define _POSIX_C_SOURCE 200809L

#include "scheduler.h"
#include <stdlib.h>     // malloc, free, realloc
#include <signal.h>     // sigaction
#include <stdio.h>
#include <time.h>
#include <sys/time.h>   // setitimer, struct itimerval


#define STACK_SIZE  (1024 * 64)  // Tamaño de pila: 64 KB
#define QUANTUM_MS   100         // Quantum de 100 milisegundos

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
    if (rr->tail == NULL) {
        rr->head = t;
    }
    else {
        rr->tail->next = t;
    }
    rr->tail = t;
}

static TCB *rr_siguiente_hilo(Scheduler *s) {
    RR_Scheduler *rr = (RR_Scheduler*)s;
    if (rr->head == NULL) {
        return NULL;
    }
    TCB *chosen = rr->head;
    rr->head    = chosen->next;
    if (rr->head == NULL) {
        rr->tail = NULL;
    }
    chosen->next  = NULL;
    chosen->state = RUNNING;
    if (rr->tail == NULL) {
        rr->head = chosen;
    }
    else {
        rr->tail->next = chosen;
    }
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
static void lottery_encolar_hilo(Scheduler *s, TCB *t) {
    Lottery_Scheduler *ls = (Lottery_Scheduler*)s;
    t->scheduler = s;
    t->state     = READY;
    t->next      = NULL;
    if (ls->head == NULL) {
        ls->head = t;
    } else {
        TCB *it = ls->head;
        while (it->next) it = it->next;
        it->next = t;
    }
}


// Selecciona el siguiente hilo por sorteo de tickets
static TCB *lottery_siguiente_hilo(Scheduler *s) {
    Lottery_Scheduler *ls = (Lottery_Scheduler*)s;
    int total = 0;
    for (TCB *it = ls->head; it; it = it->next) {
        if (it->state == READY)
            total += it->tickets;
    }
    if (total <= 0)
        return NULL;
    int winner = (rand() % total) + 1;
    int acc    = 0;
    for (TCB *it = ls->head; it; it = it->next) {
        if (it->state != READY) continue;
        acc += it->tickets;
        if (acc >= winner) {
            it->state = RUNNING;
            return it;
        }
    }
    return NULL;
}
// Inicializa el scheduler de lotería (sin preempción periódica)
void lottery_scheduler_init(Lottery_Scheduler *ls) {
    ls->base.encolar_hilo   = lottery_encolar_hilo;
    ls->base.siguiente_hilo = lottery_siguiente_hilo;
    ls->head                = NULL;
    srand((unsigned)time(NULL));
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
int main(void) {
    Lottery_Scheduler ls;                             // 68) declarar scheduler
    lottery_scheduler_init(&ls);                      // 69) inicializarlo

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