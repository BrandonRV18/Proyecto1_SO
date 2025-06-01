cmake_minimum_required(VERSION 3.30.5)
project(Proyecto1_SO C)

set(CMAKE_C_STANDARD 11)

add_executable(Proyecto1_SO
        src/scheduler.c
        src/my_pthread.c
        src/animator.c
        src/parser.c
        include/parser.h
        src/server.c
        src/cliente.c
)