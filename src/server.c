#define _POSIX_C_SOURCE 200809L


#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <math.h>
#include <time.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/time.h>
#include <x86intrin.h>
#include <ncurses.h>



#include "../include/parser.h"
#include "../include/my_pthread.h"
#ifndef MAX
#define MAX(a,b) (((a) > (b)) ? (a) : (b))
#endif
#define CPU_HZ 2800000000UL
static int *monitor_socks;
static int monitor_count;
Parser *global_cfg;
static my_mutex canvas_mutex;
static long long global_start_ms;
static Lottery_Scheduler ls;
static EDF_Scheduler edf;
static RR_Scheduler rr;
static int QUANTUM_MS = 100;


static void send_line(int sock, const char *line) {
    size_t len = strlen(line);
    ssize_t s = send(sock, line, len, 0);
    if (s < 0) {
        perror("send");
    }
}

static void send_draw(int sock, int x, int y, char c, int color_pair) {
    char buf[48];


    int n = snprintf(buf, sizeof(buf), "DRAW %d %d %c %d\n", x, y, c, color_pair);
    send_line(sock, buf);
}

static void send_refresh(int sock) {
    send_line(sock, "REFRESH\n");
}


char **rotate_ascii(char **lines, int h, int w,
                    int rotation, int *out_h, int *out_w)
{

    char **grid = malloc(sizeof(char*) * h);
    for (int i = 0; i < h; i++) {
        grid[i] = malloc(w+1);
        int len = strlen(lines[i]);
        memcpy(grid[i], lines[i], len);
        memset(grid[i] + len, ' ', w - len);
        grid[i][w] = '\0';
    }

    char **res;
    switch (rotation) {
        case   0:
            *out_h = h; *out_w = w;
            res = malloc(sizeof(char*) * h);
            for (int i = 0; i < h; i++) {
                res[i] = strdup(grid[i]);
            }
            break;

        case  90:
            *out_h = w; *out_w = h;
            res = malloc(sizeof(char*) * (*out_h));
            for (int i = 0; i < *out_h; i++) {
                res[i] = malloc(*out_w+1);
                for (int j = 0; j < *out_w; j++) {

                    res[i][j] = grid[h-1-j][i];
                }
                res[i][*out_w] = '\0';
            }
            break;

        case 180:
            *out_h = h; *out_w = w;
            res = malloc(sizeof(char*) * h);
            for (int i = 0; i < h; i++) {
                res[i] = malloc(w+1);
                for (int j = 0; j < w; j++) {
                    res[i][j] = grid[h-1-i][w-1-j];
                }
                res[i][w] = '\0';
            }
            break;

        case 270:
            *out_h = w; *out_w = h;
            res = malloc(sizeof(char*) * (*out_h));
            for (int i = 0; i < *out_h; i++) {
                res[i] = malloc(*out_w+1);
                for (int j = 0; j < *out_w; j++) {

                    res[i][j] = grid[j][w-1-i];
                }
                res[i][*out_w] = '\0';
            }
            break;

        default:

            *out_h = h; *out_w = w;
            res = malloc(sizeof(char*) * h);
            for (int i = 0; i < h; i++) {
                res[i] = strdup(grid[i]);
            }
            break;
    }


    for (int i = 0; i < h; i++) free(grid[i]);
    free(grid);
    return res;
}


static int is_position_occupied(my_mutex *mutex, int x, int y, int current_tid) {

    if (x < 0 || x >= global_cfg->width || y < 0 || y >= global_cfg->height) {
        return 1;
    }

    CanvasPosition *pos = mutex->occupied_positions;
    while (pos) {
        if (pos->x == x && pos->y == y) {

            return pos->owner_tid != current_tid;
        }
        pos = pos->next;
    }
    return 0;
}

static void occupy_position(my_mutex *mutex, int x, int y, int owner_tid) {
    CanvasPosition *new_pos = malloc(sizeof(CanvasPosition));
    new_pos->x = x;
    new_pos->y = y;
    new_pos->owner_tid = owner_tid;
    new_pos->next = mutex->occupied_positions;
    mutex->occupied_positions = new_pos;
}

static void free_position(my_mutex *mutex, int x, int y, int owner_tid) {
    CanvasPosition **ptr = &mutex->occupied_positions;
    while (*ptr) {
        if ((*ptr)->x == x && (*ptr)->y == y && (*ptr)->owner_tid == owner_tid) {
            CanvasPosition *to_free = *ptr;
            *ptr = (*ptr)->next;
            free(to_free);
            return;
        }
        ptr = &(*ptr)->next;
    }
}



static inline uint64_t rdtsc_counter(void) {
    return __rdtsc();
}

void custom_napms(uint64_t ms) {
    uint64_t start          = rdtsc_counter();
    uint64_t target         = start + (CPU_HZ / 1000ULL) * ms;
    uint64_t last_yield_ts  = start;
    uint64_t yield_interval = (CPU_HZ / 1000ULL) * 5;  // cede cada 5 ms

    while (rdtsc_counter() < target) {
        uint64_t now = rdtsc_counter();
        if (now - last_yield_ts >= yield_interval) {
            last_yield_ts = now;
            if (threadpool_alive_count() > 1) my_thread_yield();
        }

    }
}

static void switch_to_rr(void *arg) {
    (void)arg;


    printf("\n>> Cambio a Round Robin <<\n");


    static RR_Scheduler rr;
    rr_scheduler_init(&rr, 100);


    for (size_t i = 0; i < global_thread_pool.count; i++) {
        TCB *h = global_thread_pool.threads[i];
        if (h->state != TERMINATED) {
            my_thread_chsched(h, (Scheduler*)&rr);
        }
    }


    hilo_actual->state = TERMINATED;
    schedule();
}


static void switch_to_lottery(void *arg) {
    (void)arg;

    custom_napms(1500);

    printf("\n>> Cambio a Lottery <<\n");


    static Lottery_Scheduler ls;
    lottery_scheduler_init(&ls, 100);

    for (size_t i = 0; i < global_thread_pool.count; i++) {
        TCB *h = global_thread_pool.threads[i];
        if (h->state != TERMINATED) {
            my_thread_chsched(h, (Scheduler*)&ls);
        }
    }

    hilo_actual->state = TERMINATED;
    schedule();
}


void animate_shape_server(void *arg) {
    ShapeConfig *sh = (ShapeConfig *)arg;
    int dx = sh->x_end - sh->x_start;
    int dy = sh->y_end - sh->y_start;
    int steps = (int)(sqrt(dx*dx + dy*dy));



    int orig_h = sh->line_count;;
    int orig_w = 0;

    for (int k = 0; k < orig_h; k++) {
        orig_w = MAX(orig_w, (int)strlen(sh->shape_lines[k]));
    }

    struct timeval tv0;
    gettimeofday(&tv0, NULL);
    long thread_start_ms = tv0.tv_sec * 1000 + tv0.tv_usec / 1000;

    while (1) {
        struct timeval tv;
        gettimeofday(&tv, NULL);
        long now = tv.tv_sec*1000 + tv.tv_usec/1000;
        if (now - thread_start_ms >= sh->start_time) break;
        if (scheduler_activo != 1) napms(10);
        else custom_napms(10);

    }

    long deadline_ms = thread_start_ms + sh->start_time + sh->end_time;

    int prev_x = sh->x_start;
    int prev_y = sh->y_start;
    int angle_current = 0;
    int rot_h_prev, rot_w_prev;
    char **rotated_prev = rotate_ascii(
        sh->shape_lines, orig_h, orig_w,
        angle_current, &rot_h_prev, &rot_w_prev
    );


    int current_tid = hilo_actual->tid;


    for (int i = 0; i < steps; i++) {

        struct timeval tv_loop;
        gettimeofday(&tv_loop, NULL);
        long now_loop = tv_loop.tv_sec * 1000 + tv_loop.tv_usec / 1000;

        if (now_loop >= deadline_ms) {
            break;
        }



        float progress = (float)i / (float)steps;
        int x_global = sh->x_start + (int)(dx * progress);
        int y_global = sh->y_start + (int)(dy * progress);


        angle_current = (angle_current + sh->rotation) % 360;
        int rot_h, rot_w;
        char **rotated = rotate_ascii(
            sh->shape_lines, orig_h, orig_w,
            angle_current, &rot_h, &rot_w
        );


        int can_move = 1;
        my_mutex_lock(&canvas_mutex);
        for (int ly = 0; ly < rot_h && can_move; ly++) {
            for (int lx = 0; lx < rot_w; lx++) {
                if (rotated[ly][lx] != ' ') {
                    int xx = x_global + lx;
                    int yy = y_global + ly;
                    if (is_position_occupied(&canvas_mutex, xx, yy, current_tid)) {
                        can_move = 0;
                        break;
                    }
                }
            }
        }

        if (can_move) {

            for (int ly = 0; ly < rot_h_prev; ly++) {
                for (int lx = 0; lx < rot_w_prev; lx++) {
                    if (rotated_prev[ly][lx] != ' ') {
                        int xx_old = prev_x + lx;
                        int yy_old = prev_y + ly;

                        free_position(&canvas_mutex, xx_old, yy_old, current_tid);

                        int ancho_por_monitor = global_cfg->width / monitor_count;
                        int m_old = xx_old / ancho_por_monitor;
                        if (m_old < 0) m_old = 0;
                        if (m_old >= monitor_count) m_old = monitor_count - 1;
                        if (!is_position_occupied(&canvas_mutex,
                                                  prev_x + lx, prev_y + ly,
                                                  current_tid)) {
                            send_draw(monitor_socks[m_old], xx_old, yy_old, 'a', sh->color_pair);
                        }
                    }
                }
            }


            for (int ly = 0; ly < rot_h; ly++) {
                for (int lx = 0; lx < rot_w; lx++) {
                    if (rotated[ly][lx] != ' ') {
                        int xx = x_global + lx;
                        int yy = y_global + ly;
                        occupy_position(&canvas_mutex, xx, yy, current_tid);

                        int ancho_por_monitor = global_cfg->width / monitor_count;
                        int m_new = xx / ancho_por_monitor;
                        if (m_new < 0) m_new = 0;
                        if (m_new >= monitor_count) m_new = monitor_count - 1;
                        char c = rotated[ly][lx];
                        send_draw(monitor_socks[m_new], xx, yy, c, sh->color_pair);
                    }
                }
            }
            for (int m = 0; m < monitor_count; m++) {
                send_refresh(monitor_socks[m]);
            }

            prev_x = x_global;
            prev_y = y_global;

            for (int k = 0; k < rot_h_prev; k++) free(rotated_prev[k]);
            free(rotated_prev);
            rotated_prev = rotated;
            rot_h_prev   = rot_h;
            rot_w_prev   = rot_w;


        } else {

            for (int k = 0; k < rot_h; k++) free(rotated[k]);
            free(rotated);
            i--;
        }
        my_mutex_unlock(&canvas_mutex);



        if (scheduler_activo != 1) napms(50);
        else custom_napms(50);
    }
    my_mutex_lock(&canvas_mutex);
    for (int ly = 0; ly < rot_h_prev; ly++) {
        for (int lx = 0; lx < rot_w_prev; lx++) {
            if (rotated_prev[ly][lx] != ' ') {
                int xx = prev_x + lx;
                int yy = prev_y + ly;
                free_position(&canvas_mutex, xx, yy, current_tid);

                int ancho_por_monitor = global_cfg->width / monitor_count;
                int m_fin = xx / ancho_por_monitor;
                if (m_fin < 0) m_fin = 0;
                if (m_fin >= monitor_count) m_fin = monitor_count - 1;
                if (!is_position_occupied(&canvas_mutex,
                                          prev_x + lx, prev_y + ly,
                                          current_tid)) {

                    send_draw(monitor_socks[m_fin], xx, yy, 'a', sh->color_pair);

                }
            }
        }
    }

    for (int m = 0; m < monitor_count; m++) {
        send_refresh(monitor_socks[m]);
    }
    my_mutex_unlock(&canvas_mutex);

    for (int k = 0; k < rot_h_prev; k++) free(rotated_prev[k]);
    free(rotated_prev);



    my_thread_end();
}




int main(int argc, char *argv[]) {
    if (getcontext(&scheduler_ctx) == -1) {
        perror("getcontext scheduler");
        return 1;
    }

    if (argc < 2) {
        fprintf(stderr, "Uso: %s config.ini\n", argv[0]);
        return 1;
    }


    global_cfg = load_config(argv[1]);

    if (!global_cfg) {
        fprintf(stderr, "Error al cargar configuración\n");
        return 1;
    }

    load_shapes_content(global_cfg);



    monitor_count = global_cfg->monitor_count;
    monitor_socks = malloc(sizeof(int) * monitor_count);

    int server_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (server_sock < 0) { perror("socket"); return 1; }
    struct sockaddr_in addr = {
        .sin_family      = AF_INET,
        .sin_addr.s_addr = INADDR_ANY,
        .sin_port        = htons(5000)
    };
    if (bind(server_sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind"); return 1;
    }
    if (listen(server_sock, monitor_count) < 0) {
        perror("listen"); return 1;
    }
    printf("Esperando %d monitores...\n", monitor_count);


    for (int i = 0; i < monitor_count; i++) {
        struct sockaddr_in cli_addr;
        socklen_t cli_len = sizeof(cli_addr);
        monitor_socks[i] = accept(server_sock,
                                  (struct sockaddr*)&cli_addr,
                                  &cli_len);
        if (monitor_socks[i] < 0) {
            perror("accept"); return 1;
        }
        printf("→ Monitor %d conectado: %s:%d\n",
               i,
               inet_ntoa(cli_addr.sin_addr),
               ntohs(cli_addr.sin_port));
    }


    int ancho_por_monitor = global_cfg->width / monitor_count;
    for (int i = 0; i < monitor_count; i++) {
        int x_off = i * ancho_por_monitor;
        int w     = ancho_por_monitor;
        int h     = global_cfg->height;

        char region_msg[64];
        int n = snprintf(region_msg, sizeof(region_msg),
                         "REGION %d %d %d\n", x_off, w, h);
        send_line(monitor_socks[i], region_msg);
        printf("→ REGION enviado a monitor %d: %s", i, region_msg);
    }



    for (int i = 0; i < global_cfg->shape_count; i++) {

        global_cfg->shapes[i].color_pair = i + 1;
    }

    my_mutex_init(&canvas_mutex);

    edf_scheduler_init(&edf);


    struct timeval tv;
    gettimeofday(&tv, NULL);
    global_start_ms = tv.tv_sec * 1000LL + tv.tv_usec / 1000;


    for (int i = 0; i < global_cfg->shape_count; i++) {
        ShapeConfig *sh = &global_cfg->shapes[i];
        sh->start_ms = global_start_ms;

        my_thread_create(
            animate_shape_server,
            sh,
            (Scheduler*)&edf,
            sh->tickets,
            0,
            sh->end_time
        );
    }

    my_thread_create(
        switch_to_rr,
        NULL,
        (Scheduler*)&edf,
        0,
        0,
        4000
    );


    my_thread_create(
        switch_to_lottery,
        NULL,
        (Scheduler*)&edf,
        0,
        0,
        5000
    );

    TCB *first = edf.base.siguiente_hilo((Scheduler*)&edf);
    hilo_actual = first;
    swapcontext(&scheduler_ctx, &hilo_actual->context);


    for (int i = 0; i < monitor_count; i++) {
        send_line(monitor_socks[i], "END\n");
        close(monitor_socks[i]);
    }


    return 0;
}