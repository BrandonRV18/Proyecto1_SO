#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <limits.h>
#include <math.h>
#include <ncurses.h>
#include <signal.h>
#include <time.h>
#include <ucontext.h>
#include <sys/time.h>
#include "../include/my_pthread.h"
#include "../include/scheduler.h"
#include <x86intrin.h>
#include <stdint.h>

#define CPU_HZ 2800000000UL
#ifdef LINE_MAX
#undef LINE_MAX
#endif
#define LINE_MAX 256
#define INITIAL_SHAPE_CAP   4
#define INITIAL_MONITOR_CAP 4
#ifndef MAX
#define MAX(a,b) (((a) > (b)) ? (a) : (b))
#endif

static EDF_Scheduler     edf;
static Lottery_Scheduler ls;
static RR_Scheduler      rr;
static long global_start_ms;
static int QUANTUM_MS = 100;



typedef struct {
    char *name;
    char *shape_file;

    // Contenido de la forma ASCII
    char **shape_lines;
    int    line_count;
    int    line_capacity;

    int   x_start, y_start;
    int   x_end,   y_end;
    int   rotation;
    int   start_time, end_time;
    int   tickets;
    int   color_pair;
    int   tid;
    long  start_ms;
} ShapeConfig;

typedef struct {
    int width, height;
    char **monitors;
    int monitor_count, monitor_capacity;
    ShapeConfig *shapes;
    int shape_count, shape_capacity;
} Config;

static char *ltrim(char *s) { while (isspace((unsigned char)*s)) s++; return s; }
static char *rtrim(char *s) { char *e = s + strlen(s) - 1; while (e >= s && isspace((unsigned char)*e)) *e-- = '\0'; return s; }
static char *trim(char *s)  { return rtrim(ltrim(s)); }

static void init_config(Config *cfg) {
    cfg->width = cfg->height = 0;
    cfg->monitor_capacity = INITIAL_MONITOR_CAP;
    cfg->monitor_count = 0;
    cfg->monitors = malloc(sizeof(char*) * cfg->monitor_capacity);
    cfg->shape_capacity = INITIAL_SHAPE_CAP;
    cfg->shape_count = 0;
    cfg->shapes = malloc(sizeof(ShapeConfig) * cfg->shape_capacity);
}

static ShapeConfig *add_shape(Config *cfg, const char *section) {
    if (cfg->shape_count >= cfg->shape_capacity) {
        cfg->shape_capacity *= 2;
        cfg->shapes = realloc(cfg->shapes, sizeof(ShapeConfig) * cfg->shape_capacity);
    }
    ShapeConfig *sh = &cfg->shapes[cfg->shape_count];
    memset(sh, 0, sizeof(*sh));
    sh->name = strdup(section);
    cfg->shape_count++;
    return sh;
}

Config *load_config(const char *path) {
    FILE *f = fopen(path, "r"); if (!f) return NULL;
    Config *cfg = malloc(sizeof(Config)); init_config(cfg);
    char line[LINE_MAX], section[LINE_MAX] = "";
    ShapeConfig *cur = NULL;
    while (fgets(line, LINE_MAX, f)) {
        char *s = trim(line);
        if (!*s || s[0] == ';' || s[0] == '#') continue;
        if (s[0] == '[') {
            char *e = strchr(s, ']'); if (!e) continue;
            *e = '\0'; strncpy(section, s+1, sizeof(section)-1);
            if (strcmp(section, "Canvas") && strcmp(section, "Monitors")) {
                cur = add_shape(cfg, section);
            }
        } else if (cur || !strcmp(section, "Canvas") || !strcmp(section, "Monitors")) {
            char *eq = strchr(s, '='); if (!eq) continue;
            *eq = '\0'; char *k = trim(s), *v = trim(eq + 1);
            if (!strcmp(section, "Canvas")) {
                if (!strcmp(k, "width"))  cfg->width = atoi(v);
                if (!strcmp(k, "height")) cfg->height = atoi(v);
            } else if (!strcmp(section, "Monitors")) {
                if (!strcmp(k, "monitors")) {
                    char *tok = strtok(v, ",");
                    while (tok) {
                        if (cfg->monitor_count >= cfg->monitor_capacity) {
                            cfg->monitor_capacity *= 2;
                            cfg->monitors = realloc(cfg->monitors, sizeof(char*) * cfg->monitor_capacity);
                        }
                        cfg->monitors[cfg->monitor_count++] = strdup(trim(tok));
                        tok = strtok(NULL, ",");
                    }
                }
            } else {
                if (!strcmp(k, "shape_file"))     cur->shape_file = strdup(v);
                else if (!strcmp(k, "x_start"))   cur->x_start    = atoi(v);
                else if (!strcmp(k, "y_start"))   cur->y_start    = atoi(v);
                else if (!strcmp(k, "x_end"))     cur->x_end      = atoi(v);
                else if (!strcmp(k, "y_end"))     cur->y_end      = atoi(v);
                else if (!strcmp(k, "rotation"))  cur->rotation   = atoi(v);
                else if (!strcmp(k, "start_time")||!strcmp(k, "star_time")) cur->start_time = atoi(v);
                else if (!strcmp(k, "end_time"))  cur->end_time   = atoi(v);
                else if (!strcmp(k, "tickets"))   cur->tickets    = atoi(v);
            }
        }
    }
    fclose(f);
    return cfg;
}

static void load_shapes_content(Config *cfg) {
    for (int i = 0; i < cfg->shape_count; i++) {
        ShapeConfig *sh = &cfg->shapes[i];
        FILE *f = fopen(sh->shape_file, "r");
        if (!f) {
            fprintf(stderr, "No se pudo abrir shape_file %s\n", sh->shape_file);
            continue;
        }
        sh->line_capacity = 16;
        sh->line_count = 0;
        sh->shape_lines = malloc(sizeof(char*) * sh->line_capacity);

        char buf[LINE_MAX];
        while (fgets(buf, LINE_MAX, f)) {
            if (sh->line_count >= sh->line_capacity) {
                sh->line_capacity *= 2;
                sh->shape_lines = realloc(sh->shape_lines,
                                          sizeof(char*) * sh->line_capacity);
            }
            buf[strcspn(buf, "\n")] = '\0';
            sh->shape_lines[sh->line_count++] = strdup(buf);
        }
        fclose(f);
    }
}

// --- Animación con hilos y ncurses

static WINDOW *win;
static my_mutex canvas_mutex;
static Config *global_cfg = NULL;

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


// Función para verificar si un carácter de la figura colisiona
static int is_position_occupied(my_mutex *mutex, int x, int y, int current_tid) {
    // Verificar límites del canvas
    if (x < 0 || x >= global_cfg->width || y < 0 || y >= global_cfg->height) {
        return 1;
    }

    CanvasPosition *pos = mutex->occupied_positions;
    while (pos) {
        if (pos->x == x && pos->y == y) {
            // Si la posición está ocupada por otro hilo
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
        // si no toca yield, puro spin en usuario
    }
}


static void draw_explosion(int cx, int cy) {
    const int R = 3;
    int max_y, max_x;

    // 1) Bloqueamos el mutex para no interferir con otros hilos
    my_mutex_lock(&canvas_mutex);

    // 2) Obtenemos el tamaño actual de la ventana
    getmaxyx(win, max_y, max_x);

    // 3) Dibujamos los “asteriscos” de la explosión, radio a radio
    for (int r = 1; r <= R; r++) {
        // Líneas horizontales superior e inferior
        for (int dx = -r; dx <= r; dx++) {
            int row1 = cy + r + 1, col1 = cx + dx + 1;
            int row2 = cy - r + 1, col2 = cx + dx + 1;

            if (row1 >= 0 && row1 < max_y && col1 >= 0 && col1 < max_x)
                mvwaddch(win, row1, col1, '*');
            if (row2 >= 0 && row2 < max_y && col2 >= 0 && col2 < max_x)
                mvwaddch(win, row2, col2, '*');
        }
        // Líneas verticales izquierda y derecha
        for (int dy = -r; dy <= r; dy++) {
            int row3 = cy + dy + 1, col3 = cx + r + 1;
            int row4 = cy + dy + 1, col4 = cx - r + 1;

            if (row3 >= 0 && row3 < max_y && col3 >= 0 && col3 < max_x)
                mvwaddch(win, row3, col3, '*');
            if (row4 >= 0 && row4 < max_y && col4 >= 0 && col4 < max_x)
                mvwaddch(win, row4, col4, '*');
        }

        // Refrescamos y pausamos un poco para que se vea la animación
        wrefresh(win);

        if (scheduler_activo != 1) napms(50);
        else ;


    }

    // 4) Limpiamos la zona de la explosión
    for (int dy = -R; dy <= R; dy++) {
        for (int dx = -R; dx <= R; dx++) {
            int row = cy + dy + 1, col = cx + dx + 1;
            if (row >= 0 && row < max_y && col >= 0 && col < max_x)
                mvwaddch(win, row, col, ' ');
        }
    }

    wrefresh(win);

    my_mutex_unlock(&canvas_mutex);
}


void animate_shape(void *arg) {
    ShapeConfig *sh = (ShapeConfig*)arg;
    int dx    = sh->x_end   - sh->x_start;
    int dy    = sh->y_end   - sh->y_start;
    int steps = (int)(sqrt(dx*dx + dy*dy));



    int orig_h = sh->line_count;
    int orig_w = 0;
    for (int k = 0; k < orig_h; k++) {
        orig_w = MAX(orig_w, (int)strlen(sh->shape_lines[k]));
    }

    struct timeval tv0;
    gettimeofday(&tv0, NULL);
    long thread_start_ms = tv0.tv_sec*1000 + tv0.tv_usec/1000;


    while (1) {
        struct timeval tv;
        gettimeofday(&tv, NULL);
        long now = tv.tv_sec*1000 + tv.tv_usec/1000;
        if (now - thread_start_ms >= sh->start_time) break;
        if (scheduler_activo != 1) napms(10);
        else custom_napms(10);

    }


    long deadline_ms = thread_start_ms + sh->start_time + sh->end_time;

    int prev_x = sh->x_start, prev_y = sh->y_start;
    int angle_current = 0;
    int rot_h_prev, rot_w_prev;
    char **rotated_prev = rotate_ascii(
        sh->shape_lines, orig_h, orig_w,
        angle_current, &rot_h_prev, &rot_w_prev
    );

    int current_tid = hilo_actual->tid;


    my_mutex_lock(&canvas_mutex);
      wattron(win, COLOR_PAIR(sh->color_pair));
      for (int ly = 0; ly < rot_h_prev; ly++) {
        for (int lx = 0; lx < rot_w_prev; lx++) {
          char c = rotated_prev[ly][lx];
          if (c != ' ') {
            occupy_position(&canvas_mutex,
                            prev_x + lx, prev_y + ly,
                            current_tid);
            mvwaddch(win,
                     prev_y + ly + 1,
                     prev_x + lx + 1,
                     c);
          }
        }
      }
      wattroff(win, COLOR_PAIR(sh->color_pair));
      wrefresh(win);
    my_mutex_unlock(&canvas_mutex);


    for (int i = 1; i <= steps; i++) {

        struct timeval tv;
        gettimeofday(&tv, NULL);
        long now_ms = tv.tv_sec*1000 + tv.tv_usec/1000;
        if (now_ms >= deadline_ms) {

            draw_explosion(prev_x + rot_w_prev/2,
                           prev_y + rot_h_prev/2);
            break;
        }


        float progress = (float)i / steps;
        int x = sh->x_start + (int)(dx * progress);
        int y = sh->y_start + (int)(dy * progress);


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
                if (rotated[ly][lx] != ' ' &&
                    is_position_occupied(&canvas_mutex,
                                         x + lx, y + ly,
                                         current_tid)) {
                    can_move = 0;
                    break;
                }
            }
        }

        if (can_move) {

            for (int ly = 0; ly < rot_h_prev; ly++) {
                for (int lx = 0; lx < rot_w_prev; lx++) {
                    if (rotated_prev[ly][lx] != ' ') {
                        free_position(&canvas_mutex,
                                      prev_x + lx, prev_y + ly,
                                      current_tid);
                        if (!is_position_occupied(&canvas_mutex,
                                                  prev_x + lx, prev_y + ly,
                                                  current_tid)) {
                            mvwaddch(win,
                                     prev_y + ly + 1,
                                     prev_x + lx + 1,
                                     ' ');
                        }
                    }
                }
            }


            wattron(win, COLOR_PAIR(sh->color_pair));
            for (int ly = 0; ly < rot_h; ly++) {
                for (int lx = 0; lx < rot_w; lx++) {
                    char c = rotated[ly][lx];
                    if (c != ' ') {
                        occupy_position(&canvas_mutex,
                                        x + lx, y + ly,
                                        current_tid);
                        mvwaddch(win,
                                 y + ly + 1,
                                 x + lx + 1,
                                 c);
                    }
                }
            }
            wattroff(win, COLOR_PAIR(sh->color_pair));
            wrefresh(win);


            prev_x = x;  prev_y = y;
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

        if (scheduler_activo != 1) napms(100);
        else custom_napms(100);



    }


    my_mutex_lock(&canvas_mutex);
    for (int ly = 0; ly < rot_h_prev; ly++) {
        for (int lx = 0; lx < rot_w_prev; lx++) {
            if (rotated_prev[ly][lx] != ' ') {
                free_position(&canvas_mutex,
                              prev_x + lx, prev_y + ly,
                              current_tid);
                if (!is_position_occupied(&canvas_mutex,
                                          prev_x + lx, prev_y + ly,
                                          current_tid)) {
                    mvwaddch(win,
                             prev_y + ly + 1,
                             prev_x + lx + 1,
                             ' ');
                }
            }
        }
    }
    wrefresh(win);
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


    initscr(); cbreak(); noecho();

    start_color();


    int basic_colors[] = {
        COLOR_RED, COLOR_GREEN, COLOR_YELLOW,
        COLOR_BLUE, COLOR_MAGENTA, COLOR_CYAN,
        COLOR_WHITE
    };
    int n_colors = sizeof(basic_colors)/sizeof(*basic_colors);

    for (int i = 0; i < global_cfg->shape_count; i++) {
        int pair = i + 1;  // los pares empiezan en 1
        int fg = basic_colors[i % n_colors];
        init_pair(pair, fg, COLOR_BLACK);
        global_cfg->shapes[i].color_pair = pair;
    }


    init_pair(10, COLOR_CYAN, COLOR_BLACK);


    win = newwin(global_cfg->height, global_cfg->width, 0, 0);


    wattron(win, COLOR_PAIR(10));
    box(win, 0, 0);
    wattroff(win, COLOR_PAIR(10));
    wrefresh(win);
    my_mutex_init(&canvas_mutex);



    edf_scheduler_init(&edf);
    //lottery_scheduler_init(&ls, QUANTUM_MS);
    //rr_scheduler_init(&rr, QUANTUM_MS);


    struct timeval tv;
    gettimeofday(&tv, NULL);
    global_start_ms = tv.tv_sec*1000 + tv.tv_usec/1000;


    for (int i = 0; i < global_cfg->shape_count; i++) {
        ShapeConfig *sh = &global_cfg->shapes[i];

        my_thread_create(
            animate_shape,
            sh,
            (Scheduler*)&edf,
            sh->tickets,
            0,
            sh->end_time
        );
        sh->start_ms = global_start_ms;
    }




    TCB *first = edf.base.siguiente_hilo((Scheduler*)&edf);


    hilo_actual = first;
    swapcontext(&scheduler_ctx, &hilo_actual->context);



    getch();


    endwin();


    return 0;

}
