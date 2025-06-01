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

/**
 * Scheduler
 *
 * Representa la interfaz genérica de un scheduler para manejo de hilos.
 *
 * Campos:
 *   void (*encolar_hilo)(Scheduler *self, TCB *t):
 *     – puntero a la función que encola un hilo (TCB) en la estructura interna.
 *
 *   TCB *(*siguiente_hilo)(Scheduler *self):
 *     – puntero a la función que devuelve el siguiente hilo listo para ejecutar.
 *
 *   void (*remover_hilo)(Scheduler *self, TCB *t):
 *     – puntero a la función que remueve un hilo (TCB) de la estructura interna.
 */
struct Scheduler {
    void   (*encolar_hilo)(Scheduler *self, TCB *t);
    TCB   *(*siguiente_hilo)(Scheduler *self);
    void   (*remover_hilo)   (Scheduler *self, TCB *t);
};


/**
 * TCB (Thread Control Block)
 *
 * Contiene la información de control de un hilo, incluyendo estado, contexto
 * de ejecución y datos específicos para distintos tipos de scheduler.
 *
 * Campos:
 *   int tid:
 *     – identificador único del hilo.
 *
 *   ucontext_t context:
 *     – contexto de usuario que almacena registros y stack pointer para
 *       cambio de contexto (swapcontext).
 *
 *   ThreadState state:
 *     – estado actual del hilo (READY, RUNNING, TERMINATED, BLOCKED, etc.).
 *
 *   Scheduler *scheduler:
 *     – puntero al scheduler al que pertenece este hilo.
 *
 *   void *stack:
 *     – puntero a la memoria asignada para la pila del hilo.
 *
 *   TCB *next:
 *     – puntero al siguiente bloque de control en la lista/cola del scheduler.
 *
 *   int tickets:
 *     – número de boletos asignados en un scheduler Lottery; determina la
 *       probabilidad de ser elegido.
 *
 *   int priority:
 *     – prioridad del hilo (usada si se extiende para schedulers por prioridad).
 *
 *   long deadline:
 *     – marca de tiempo límite en milisegundos (usada por scheduler EDF).
 *
 *   TCB *joiner:
 *     – puntero al hilo que está esperando al hilo actual en una operación join.
 *
 *   int detached:
 *     – indicador (0/1) de si el hilo está detached (desvinculado para que
 *       su terminación libere automáticamente recursos).
 */
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


/**
 * RR_Scheduler
 *
 * Scheduler de tipo Round Robin: mantiene una cola de hilos y un quantum de tiempo.
 *
 * Campos:
 *   Scheduler base:
 *     – parte común de la interfaz (punteros a funciones encolar, siguiente y remover).
 *
 *   int quantum:
 *     – duración del quantum en milisegundos para preempción con setitimer.
 *
 *   TCB *head:
 *     – puntero al primer hilo en la cola Round Robin.
 *
 *   TCB *tail:
 *     – puntero al último hilo en la cola Round Robin.
 */
struct RR_Scheduler {
    Scheduler base;
    int       quantum;
    TCB      *head;
    TCB      *tail;
};


/**
 * Lottery_Scheduler
 *
 * Scheduler de tipo Lottery: mantiene una lista enlazada de hilos con boletos
 * y selecciona aleatoriamente según el número de tickets.
 *
 * Campos:
 *   Scheduler base:
 *     – parte común de la interfaz (punteros a funciones encolar, siguiente y remover).
 *
 *   TCB *head:
 *     – puntero al primer hilo en la lista de Lottery.
 *
 *   int quantum:
 *     – duración del quantum en milisegundos para preempción (puede coincidir
 *       con Round Robin en ejecución por tiempo fijo antes de sortear otra lotería).
 */
struct Lottery_Scheduler {
    Scheduler base;
    TCB      *head;
    int quantum;
};




/**
 * EDF_Scheduler
 *
 * Scheduler de tipo EDF (Earliest Deadline First): mantiene una lista enlazada
 * de hilos y siempre elige para ejecución aquel con el deadline más cercano.
 *
 * Campos:
 *   Scheduler base:
 *     – parte común de la interfaz (punteros a funciones encolar, siguiente y remover).
 *
 *   TCB *head:
 *     – puntero al primer hilo en la lista de hilos manejados por EDF.
 */
struct EDF_Scheduler {
    Scheduler base;
    TCB      *head;
};


/**
 * ThreadPool
 *
 * Administrador de un conjunto de hilos (TCB) para facilitar
 * creación, registro y manejo global.
 *
 * Campos:
 *   size_t created_threads_counter:
 *     – contador total de hilos creados (se puede usar como base para tid).
 *
 *   TCB **threads:
 *     – arreglo dinámico de punteros a TCB, representando todos los hilos registrados.
 *
 *   size_t count:
 *     – número actual de hilos almacenados en el arreglo.
 *
 *   size_t capacity:
 *     – capacidad máxima actual del arreglo, cuando count + 1 > capacity,
 *       se expande el arreglo (realloc).
 */
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
extern int scheduler_activo;


int    registrar_hilo(ThreadPool *p, TCB *t);
int    my_thread_chsched(TCB *t, Scheduler *new_sch);
TCB   *buscar_hilo_id(ThreadPool *p, int tid);
void   encolar_hilo(Scheduler *sched, TCB *t);
void   schedule(void);
void print_all_rr_snapshots(void);
int threadpool_alive_count(void);

void   rr_scheduler_init(RR_Scheduler *rr, int quantum_ms);
void   lottery_scheduler_init(Lottery_Scheduler *ls, int quantum_ms);
void   edf_scheduler_init(EDF_Scheduler *es);

#endif
