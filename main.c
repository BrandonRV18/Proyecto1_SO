#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>     // malloc, free
#include <ucontext.h>   // para ucontext_t
#include <stddef.h>     // para size_t
#include <signal.h>     // sigaction
#include <sys/time.h>   // setitimer, struct itimerval

typedef enum {
    READY,        // Listo para ejecutarse
    RUNNING,      // Actualmente en ejecución
    BLOCKED,      // Bloqueado (p. ej. espera de mutex o join)
    TERMINATED    // Ha terminado su ejecución
} ThreadState;

int next_tid=0; //Variable para luego poder asignar un id a cada hilo

#define STACK_SIZE  (1024 * 64)  // Tamaño de pila: 64 KB
#define QUANTUM_MS   100         // Quantum de 100 milisegundos


typedef struct Scheduler    Scheduler;
typedef struct TCB          TCB;
typedef struct RR_Scheduler RR_Scheduler;




// -------------------------------------------------------------
// Thread Control Block (TCB)
// Representa toda la información de un hilo en espacio de usuario
// -------------------------------------------------------------
struct TCB {
    int            tid;         // Identificador único del hilo
    ucontext_t     context;     // Contexto de CPU (registros, PC, SP)
    ThreadState    state;       // Estado actual del hilo
    Scheduler     *scheduler;   // Estrategia de scheduling asignada
    void          *stack;       // Puntero a la pila propia (malloc)
    TCB           *next;        // Enlace genérico para colas (ready, etc.)

    int            tickets;     // Numero de tickets (Lottery Scheduling)
    int            priority;    // Prioridad (Real-Time Scheduling)
    long           deadline;    // Deadline (timestamp en milisegundos)
};

/* -------------------------------------------------------------
   ThreadPool unificado: un solo vector de TCB*
------------------------------------------------------------- */
typedef struct {
    size_t created_threads_counter;  // sigue útil para asignar tid al crear
    TCB   **threads;   // array dinámico de punteros a TODOS los TCB
    size_t  count;     // cuántos hay
    size_t  capacity;  // capacidad asignada
} ThreadPool;

/* -------------------------------------------------------------
   Asegura espacio para al menos un TCB más
------------------------------------------------------------- */
static void ensure_capacity(ThreadPool *p) {
    if (p->count + 1 > p->capacity) {
        size_t new_cap = (p->capacity == 0 ? 4 : p->capacity * 2);
        p->threads = realloc(p->threads, new_cap * sizeof(TCB *));
        p->capacity = new_cap;
    }
}

/* -------------------------------------------------------------
   Registra un TCB* en el pool unificado
   Asume que t->tid ya fue inicializado en my_thread_create()
------------------------------------------------------------- */
int registrar_hilo(ThreadPool *p, TCB *t) {
    ensure_capacity(p);
    p->threads[p->count++] = t;
    return t->tid;
}

/* -------------------------------------------------------------
   Busca un hilo por su tid en el pool unificado
   Devuelve NULL si no existe
------------------------------------------------------------- */
TCB *buscar_hilo_id(ThreadPool *p, int tid) {
    for (size_t i = 0; i < p->count; i++) {
        if (p->threads[i]->tid == tid) {
            return p->threads[i];
        }
    }
    return NULL;
}



// -------------------------------------------------------------
// Variable global que señala el TCB que está ejecutándose
// -------------------------------------------------------------
static TCB *hilo_actual = NULL;

// -------------------------------------------------------------
// Interfaz genérica Scheduler
// Cada política implementará estos dos métodos
// -------------------------------------------------------------
struct Scheduler {
    // Inserta un hilo en la cola de listos
    void   (*encolar_hilo)(Scheduler *self, TCB *t);
    // Devuelve el siguiente hilo a ejecutar
    TCB   *(*siguiente_hilo)(Scheduler *self);
};

// -------------------------------------------------------------
// schedule()
// Función central de preempción: elige y ejecuta el siguiente hilo
// -------------------------------------------------------------
void schedule(void) {
    // Si no hay hilo en ejecución, no hacemos nada
    if (hilo_actual == NULL) {
        return;
    }

    // Guardamos el hilo saliente
    TCB *prev = hilo_actual;
    // Obtenemos su scheduler (Round Robin, Lottery, etc.)
    Scheduler *sch = prev->scheduler;
    // Pedimos el siguiente hilo listo
    TCB *next = sch->siguiente_hilo(sch);

    // Si no hay hilos listos, conservamos el actual
    if (next == NULL) {
        return;
    }

    // Actualizamos hilo en ejecución y cambiamos contexto
    hilo_actual = next;
    swapcontext(&prev->context, &next->context);
}

// -------------------------------------------------------------
// Manejador de SIGALRM
// Se llama automáticamente cada quantum_ms
// -------------------------------------------------------------
static void alarm_handler(int sig) {
    (void)sig;   // Evitar advertencia de parámetro sin usar
    schedule();
}


// -------------------------------------------------------------
// start_preemption()
// Configura el temporizador y el handler para preempción real
// -------------------------------------------------------------
static void start_preemption(int quantum_ms) {
    //Instalar handler de SIGALRM
    struct sigaction sa;
    sigemptyset(&sa.sa_mask);
    sa.sa_handler = alarm_handler;
    sa.sa_flags   = SA_RESTART;
    sigaction(SIGALRM, &sa, NULL);

    //Configurar setitimer para ITIMER_REAL
    struct itimerval timer;
    timer.it_interval.tv_sec  = quantum_ms / 1000;
    timer.it_interval.tv_usec = (quantum_ms % 1000) * 1000;
    timer.it_value.tv_sec     = timer.it_interval.tv_sec;
    timer.it_value.tv_usec    = timer.it_interval.tv_usec;
    setitimer(ITIMER_REAL, &timer, NULL);
}

// -------------------------------------------------------------
// RR_Scheduler: implementación concreta de Scheduler para Round Robin
// -------------------------------------------------------------
struct RR_Scheduler {
    Scheduler base;    // “Hereda” los métodos de Scheduler
    int       quantum; // Quantum en milisegundos (no usado internamente)
    TCB      *head;    // Frente de la cola FIFO de listos
    TCB      *tail;    // Final de la cola FIFO de listos
};

// -------------------------------------------------------------
// rr_encolar_hilo()
// Inserta un TCB al final de la cola de Round Robin
// -------------------------------------------------------------
static void rr_encolar_hilo(Scheduler *s, TCB *t) {
    RR_Scheduler *rr = (RR_Scheduler *)s;

    // Asociamos el scheduler a este hilo
    t->scheduler = s;
    // Marcamos hilo como listo
    t->state     = READY;
    // No hay siguiente (será el último)
    t->next      = NULL;

    // Si la cola está vacía, cabeza = t; si no, enlazamos al final
    if (rr->tail == NULL) {
        rr->head = t;
    } else {
        rr->tail->next = t;
    }
    rr->tail = t;
}

// -------------------------------------------------------------
// rr_siguiente_hilo()
// Saca el frente, lo reencola al final y lo devuelve
// -------------------------------------------------------------
static TCB *rr_siguiente_hilo(Scheduler *s) {
    RR_Scheduler *rr = (RR_Scheduler *)s;

    // Si no hay hilos listos, devolvemos NULL
    if (rr->head == NULL) {
        return NULL;
    }

    // Tomamos el primer hilo de la cola
    TCB *chosen = rr->head;
    rr->head    = chosen->next;

    // Si la cola queda vacía tras quitar chosen, reseteamos tail
    if (rr->head == NULL) {
        rr->tail = NULL;
    }

    // Desconectamos chosen de la lista
    chosen->next  = NULL;
    // Marcamos que está en ejecución
    chosen->state = RUNNING;

    // Reencolamos chosen al final de la cola
    if (rr->tail == NULL) {
        rr->head = chosen;
    } else {
        rr->tail->next = chosen;
    }
    rr->tail = chosen;

    return chosen;
}

// -------------------------------------------------------------
// rr_scheduler_init()
// Inicializa RR_Scheduler y arranca el temporizador
// -------------------------------------------------------------
void rr_scheduler_init(RR_Scheduler *rr, int quantum_ms) {
    // Vinculamos las implementaciones de la interfaz
    rr->base.encolar_hilo   = rr_encolar_hilo;
    rr->base.siguiente_hilo = rr_siguiente_hilo;
    // Guardamos el quantum
    rr->quantum             = quantum_ms;
    // Cola inicialmente vacía
    rr->head = rr->tail = NULL;
    // Arrancamos la preempción
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


// -------------------------------------------------------------
// main()
// Crea tres hilos de ejemplo y arranca el scheduler Round Robin
// -------------------------------------------------------------
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