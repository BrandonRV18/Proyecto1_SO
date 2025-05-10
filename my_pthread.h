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


#endif
