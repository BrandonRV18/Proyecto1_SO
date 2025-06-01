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


/**
 * send_line
 *
 * Envía una línea de texto completa (terminada en '\n') al socket indicado.
 * Si ocurre un error al enviar, imprime el mensaje de error.
 *
 * Entradas:
 *   sock – descriptor del socket al que se desea enviar.
 *   line – cadena de caracteres que se enviará (debe terminar en '\n').
 *
 * Retorna:
 *   void
 */
static void send_line(int sock, const char *line) {
    size_t len = strlen(line);
    ssize_t s = send(sock, line, len, 0);
    if (s < 0) {
        perror("send");
    }
}

/**
 * send_draw
 *
 * Construye y envía un comando "DRAW" al socket para dibujar un carácter
 * en las coordenadas globales (x, y) con el par de color indicado.
 *
 * Entradas:
 *   sock       – descriptor del socket donde se enviará el comando.
 *   x          – coordenada global en el eje X donde se dibujará.
 *   y          – coordenada global en el eje Y donde se dibujará.
 *   c          – carácter a dibujar en la posición (x, y).
 *   color_pair – índice del par de colores configurado en ncurses.
 *
 * Retorna:
 *   void
 */
static void send_draw(int sock, int x, int y, char c, int color_pair) {
    char buf[48];


    int n = snprintf(buf, sizeof(buf), "DRAW %d %d %c %d\n", x, y, c, color_pair);
    send_line(sock, buf);
}

/**
 * send_refresh
 *
 * Envía el comando "REFRESH" al socket para indicar que la ventana del monitor
 * debe refrescarse y mostrar los cambios acumulados.
 *
 * Entradas:
 *   sock – descriptor del socket al que se enviará el comando.
 *
 * Retorna:
 *   void
 */
static void send_refresh(int sock) {
    send_line(sock, "REFRESH\n");
}


/**
 * rotate_ascii
 *
 * Genera una nueva matriz de cadenas (grid rotada) a partir de la matriz de líneas ASCII
 * original, aplicando rotación en ángulos multiples de 90 grados. La función retorna
 * la matriz rotada y actualiza out_h y out_w con las dimensiones del resultado.
 *
 * Entradas:
 *   lines    – arreglo de cadenas que representan las líneas ASCII originales.
 *   height        – número de líneas (altura) de la forma original.
 *   width        – ancho máximo (número de columnas) de la forma original.
 *   rotation – ángulo de rotación en grados (0, 90, 180 o 270). Cualquier otro valor
 *              retorna la forma sin rotar.
 *   out_h    – puntero a entero donde se guardará la nueva altura tras rotar.
 *   out_w    – puntero a entero donde se guardará el nuevo ancho tras rotar.
 *
 * Retorna:
 *   char** – matriz dinámica de cadenas (cada una terminada en '\0') con la forma rotada.
 *
 */
char **rotate_ascii(char **lines, int height, int width,
                    int rotation, int *out_h, int *out_w)
{

    char **grid = malloc(sizeof(char*) * height);
    for (int i = 0; i < height; i++) {
        grid[i] = malloc(width+1);
        int len = strlen(lines[i]);
        memcpy(grid[i], lines[i], len);
        memset(grid[i] + len, ' ', width - len);
        grid[i][width] = '\0';
    }

    char **res;
    switch (rotation) {
        case   0:
            *out_h = height; *out_w = width;
            res = malloc(sizeof(char*) * height);
            for (int i = 0; i < height; i++) {
                res[i] = strdup(grid[i]);
            }
            break;

        case  90:
            *out_h = width; *out_w = height;
            res = malloc(sizeof(char*) * (*out_h));
            for (int i = 0; i < *out_h; i++) {
                res[i] = malloc(*out_w+1);
                for (int j = 0; j < *out_w; j++) {

                    res[i][j] = grid[height-1-j][i];
                }
                res[i][*out_w] = '\0';
            }
            break;

        case 180:
            *out_h = height; *out_w = width;
            res = malloc(sizeof(char*) * height);
            for (int i = 0; i < height; i++) {
                res[i] = malloc(width+1);
                for (int j = 0; j < width; j++) {
                    res[i][j] = grid[height-1-i][width-1-j];
                }
                res[i][width] = '\0';
            }
            break;

        case 270:
            *out_h = width; *out_w = height;
            res = malloc(sizeof(char*) * (*out_h));
            for (int i = 0; i < *out_h; i++) {
                res[i] = malloc(*out_w+1);
                for (int j = 0; j < *out_w; j++) {

                    res[i][j] = grid[j][width-1-i];
                }
                res[i][*out_w] = '\0';
            }
            break;

        default:

            *out_h = height; *out_w = width;
            res = malloc(sizeof(char*) * height);
            for (int i = 0; i < height; i++) {
                res[i] = strdup(grid[i]);
            }
            break;
    }


    for (int i = 0; i < height; i++) free(grid[i]);
    free(grid);
    return res;
}

/**
 * is_position_occupied
 *
 * Verifica si la posición global (x, y) está ocupada en el canvas por un hilo distinto
 * a current_tid o si está fuera de los límites definidos en global_cfg.
 *
 * Entradas:
 *   mutex       – puntero al mutex que contiene la lista enlazada de CanvasPosition.
 *   x           – coordenada global en el eje X.
 *   y           – coordenada global en el eje Y.
 *   current_tid – identificador (tid) del hilo que desea ocupar esa posición.
 *
 * Retorna:
 *   int – 1 si la posición está ocupada (o fuera de rango), 0 si está libre o pertenece
 *         al mismo hilo current_tid.
 */
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

/**
 * occupy_position
 *
 * Marca la posición global (x, y) como ocupada en el mutex por el hilo identificado
 * con owner_tid. Inserta una nueva posición al inicio de la lista de posiciones ocupadas.
 *
 * Entradas:
 *   mutex     – puntero al mutex que contiene la lista de CanvasPosition.
 *   x         – coordenada global en el eje X que se desea marcar.
 *   y         – coordenada global en el eje Y que se desea marcar.
 *   owner_tid – identificador (tid) del hilo que ocupa esa posición.
 *
 * Retorna:
 *   void
 */
static void occupy_position(my_mutex *mutex, int x, int y, int owner_tid) {
    CanvasPosition *new_pos = malloc(sizeof(CanvasPosition));
    new_pos->x = x;
    new_pos->y = y;
    new_pos->owner_tid = owner_tid;
    new_pos->next = mutex->occupied_positions;
    mutex->occupied_positions = new_pos;
}


/**
 * free_position
 *
 * Libera la marca de ocupación de la posición (x, y) para el hilo owner_tid en el mutex.
 * Busca en la lista enlazada y remueve el nodo correspondiente.
 *
 * Entradas:
 *   mutex     – puntero al mutex que contiene la lista de CanvasPosition.
 *   x         – coordenada global en el eje X que se desea liberar.
 *   y         – coordenada global en el eje Y que se desea liberar.
 *   owner_tid – identificador (tid) del hilo que liberó esa posición.
 *
 * Retorna:
 *   void
 */
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


/**
 * rdtsc_counter
 *
 * Retorna el valor actual del contador de ciclos de la CPU usando la instrucción
 * RDTSC. Este valor se utiliza para medir tiempos de alta resolución.
 *
 * Entradas:
 *   ninguna
 *
 * Retorna:
 *   uint64_t – valor de lectura del contador de tiempo de CPU.
 */
static inline uint64_t rdtsc_counter(void) {
    return __rdtsc();
}


/**
 * custom_napms
 *
 * Suspende la ejecución del hilo activo durante aproximadamente 'ms' milisegundos,
 * usando el contador de CPU (RDTSC) para medir el tiempo transcurrido. Internamente,
 * cede el procesamiento cada 5 ms llamando a my_thread_yield() si hay más de un hilo vivo.
 *
 * Entradas:
 *   ms – número de milisegundos que se desea dormir el hilo actual.
 *
 * Retorna:
 *   void
 */
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

/**
 * switch_to_rr
 *
 * Función que cambia el planificador de todos los hilos vivos al Scheduler Round Robin (RR)
 * con quantum de 100 ms. Una vez reasignados, marca el hilo actual como TERMINATED y llama a schedule()
 * para ceder el control al siguiente hilo disponible.
 *
 * Entradas:
 *   arg
 *
 * Retorna:
 *   void
 */
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


/**
 * switch_to_lottery
 *
 * Función que espera 1500 ms (usando custom_napms), luego cambia el planificador
 * de todos los hilos vivos al Scheduler Lottery con quantum de 100 tickets. Después
 * marca el hilo actual como TERMINATED y llama a schedule() para ceder el control.
 *
 * Entradas:
 *   arg
 *
 * Retorna:
 *   void
 */
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

/**
 * animate_shape_server
 *
 * Ejecuta la animación de una forma ASCII en el servidor, enviando comandos a múltiples monitores.
 * La animación:
 *   1) Espera hasta sh->start_time antes de comenzar (usando napms custom).
 *   2) Calcula la trayectoria lineal desde (x_start, y_start) hasta (x_end, y_end).
 *   3) En cada paso:
 *        - Rota la forma según sh->rotation.
 *        - Verifica con is_position_occupied() si la posición siguiente está libre.
 *        - Si puede moverse:
 *            a) Libera ocupación y envía comandos DRAW con carácter 'a' (espacio coloreado)
 *               para borrar la forma anterior en cada monitor correspondiente.
 *            b) Asigna nuevas posiciones como ocupadas y envía comandos DRAW para la nueva forma.
 *            c) Envía REFRESH a todos los monitores.
 *            d) Actualiza prev_x, prev_y y libera memoria de la forma previa rotada.
 *        - Si no puede moverse, descarta la forma rotada actual y repite el paso anterior (i--).
 *        - Cede procesamiento con napms o custom_napms (50 ms) según scheduler_activo.
 *   4) Tras finalizar todos los pasos o llegar a deadline, borra la forma final:
 *        - Libera ocupaciones (free_position) y envía DRAW 'a' para borrar en cada monitor.
 *        - Envía REFRESH final a todos los monitores.
 *        - Libera memoria de la última forma rotada.
 *   5) Llama a my_thread_end() para terminar el hilo.
 *
 * Entradas:
 *   arg – puntero a ShapeConfig que contiene parámetros de animación (coordenadas, tiempos, tickets, color_pair).
 *
 * Retorna:
 *   void
 */
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
    int current_angle = 0;
    int rot_h_prev, rot_w_prev;
    char **rotated_prev = rotate_ascii(
        sh->shape_lines, orig_h, orig_w,
        current_angle, &rot_h_prev, &rot_w_prev
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


        current_angle = (current_angle + sh->rotation) % 360;
        int rot_h, rot_w;
        char **rotated = rotate_ascii(
            sh->shape_lines, orig_h, orig_w,
            current_angle, &rot_h, &rot_w
        );


        int can_move = 1;
        my_mutex_lock(&canvas_mutex);
        for (int row = 0; row < rot_h && can_move; row++) {
            for (int col = 0; col < rot_w; col++) {
                if (rotated[row][col] != ' ') {
                    int xx = x_global + col;
                    int yy = y_global + row;
                    if (is_position_occupied(&canvas_mutex, xx, yy, current_tid)) {
                        can_move = 0;
                        break;
                    }
                }
            }
        }

        if (can_move) {

            for (int row = 0; row < rot_h_prev; row++) {
                for (int col = 0; col < rot_w_prev; col++) {
                    if (rotated_prev[row][col] != ' ') {
                        int xx_old = prev_x + col;
                        int yy_old = prev_y + row;

                        free_position(&canvas_mutex, xx_old, yy_old, current_tid);

                        int ancho_por_monitor = global_cfg->width / monitor_count;
                        int m_old = xx_old / ancho_por_monitor;
                        if (m_old < 0) m_old = 0;
                        if (m_old >= monitor_count) m_old = monitor_count - 1;
                        if (!is_position_occupied(&canvas_mutex,
                                                  prev_x + col, prev_y + row,
                                                  current_tid)) {
                            send_draw(monitor_socks[m_old], xx_old, yy_old, 'a', sh->color_pair);
                        }
                    }
                }
            }


            for (int row = 0; row < rot_h; row++) {
                for (int col = 0; col < rot_w; col++) {
                    if (rotated[row][col] != ' ') {
                        int xx = x_global + col;
                        int yy = y_global + row;
                        occupy_position(&canvas_mutex, xx, yy, current_tid);

                        int ancho_por_monitor = global_cfg->width / monitor_count;
                        int m_new = xx / ancho_por_monitor;
                        if (m_new < 0) m_new = 0;
                        if (m_new >= monitor_count) m_new = monitor_count - 1;
                        char c = rotated[row][col];
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
    for (int row = 0; row < rot_h_prev; row++) {
        for (int col = 0; col < rot_w_prev; col++) {
            if (rotated_prev[row][col] != ' ') {
                int xx = prev_x + col;
                int yy = prev_y + row;
                free_position(&canvas_mutex, xx, yy, current_tid);

                int ancho_por_monitor = global_cfg->width / monitor_count;
                int m_fin = xx / ancho_por_monitor;
                if (m_fin < 0) m_fin = 0;
                if (m_fin >= monitor_count) m_fin = monitor_count - 1;
                if (!is_position_occupied(&canvas_mutex,
                                          prev_x + col, prev_y + row,
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



/**
 * main
 *
 * Punto de entrada de la aplicación servidor que:
 *   1) Carga configuración INI con load_config().
 *   2) Carga contenido de formas con load_shapes_content().
 *   3) Crea sockets y espera conexiones de monitores (monitor_count conexiones).
 *   4) Envía a cada monitor su región (REGION x_off w h).
 *   5) Asigna un par de colores (color_pair) distinto a cada ShapeConfig.
 *   6) Inicializa el mutex del canvas y el scheduler EDF.
 *   7) Crea hilos para cada forma (animate_shape_server) y dos hilos extra que
 *      cambiarán el planificador a RR y a Lottery en tiempos específicos.
 *   8) Inicia la primera rutina del scheduler EDF y cede el contexto al primer hilo.
 *   9) Al terminar todos los hilos, envía "END" a cada monitor y cierra los sockets.
 *
 * Entradas:
 *   argc, argv:
 *     argc debe ser ≥ 2 (nombre_programa, config.ini).
 *     argv[1] = ruta al archivo de configuración INI.
 *
 * Retorna:
 *   int – 0 si finaliza correctamente, 1 si ocurre algún error en:
 */
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