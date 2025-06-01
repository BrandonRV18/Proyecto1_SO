#ifndef MY_PTHREAD_H
#define MY_PTHREAD_H

#include "scheduler.h"

int   my_thread_create(void (*func)(void*),
                       void *arg,
                       Scheduler *sched,
                       int tickets,
                       int priority,
                       long deadline);
void  my_thread_end(void);
void  my_thread_yield(void);
void  my_thread_join(int tid);
int   my_thread_detach(int tid);

typedef struct canvas_position {
    int x;
    int y;
    int owner_tid;
    struct canvas_position *next;
} CanvasPosition;


typedef struct my_mutex {
    int bloqueado;
    TCB *propietario;
    TCB *head;
    TCB *tail;
    CanvasPosition *occupied_positions;
} my_mutex;

/* -------------------------------------------------------------
   Prototipos de la API de mutex
------------------------------------------------------------- */
int my_mutex_init(my_mutex *m);
int my_mutex_destroy(my_mutex *m);
int my_mutex_lock(my_mutex *m);
int my_mutex_trylock(my_mutex *m);
int my_mutex_unlock(my_mutex *m);



#endif
