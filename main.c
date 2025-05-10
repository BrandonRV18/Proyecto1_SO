#include <stdio.h>

#include <ucontext.h>   // para ucontext_t
#include <stddef.h>     // para size_t

/* Estado de los hilos */
typedef enum { READY, RUNNING, BLOCKED, TERMINATED } ThreadState;


/* El TCB en sí mismo */
typedef struct TCB {
    int              tid;         // Identificador único del hilo
    ucontext_t       context;     // Contexto de CPU (registros, PC, SP)
    ThreadState      state;       // READY, RUNNING, BLOCKED o TERMINATED
    struct Scheduler *scheduler;  // Puntero a la estrategia de scheduling actual
    void             *stack;      // Puntero al bloque de memoria reservado como pila
    struct TCB       *next;       // Enlace genérico para insertarlo en colas



    int              tickets;     // Número de tickets para LotteryScheduling
    int              priority;    // Prioridad para RealTimeScheduling
    long             deadline;    // Deadline (timestamp) para RT


} TCB;





int main(void) {
    printf("Hello, World!\n");
    return 0;
}