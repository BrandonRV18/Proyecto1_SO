cmake_minimum_required(VERSION 3.30.5)
project(Proyecto1_SO C)

set(CMAKE_C_STANDARD 11)

add_executable(Proyecto1_SO
        main.c
        scheduler.c
        my_pthread.c
        animator.c
)