#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#define LINE_MAX 256
#define INITIAL_SHAPE_CAP   4
#define INITIAL_MONITOR_CAP 4

// Descripción de una figura animada
typedef struct {
    char *name;
    char *shape_file;
    int   x_start, y_start;
    int   x_end,   y_end;
    int   rotation;
    int   start_time, end_time;
    char  scheduler[32];
    int   tickets;
    int   deadline;
} ShapeConfig;

// Configuración global
typedef struct {
    int    width, height;
    char **monitors;
    int    monitor_count, monitor_capacity;

    ShapeConfig *shapes;
    int          shape_count, shape_capacity;
} Config;

// Funciones auxiliares para trim:
static char *ltrim(char *s) {
    while (isspace((unsigned char)*s)) s++;
    return s;
}
static char *rtrim(char *s) {
    char *end = s + strlen(s) - 1;
    while (end >= s && isspace((unsigned char)*end)) *end-- = '\0';
    return s;
}
static char *trim(char *s) {
    return rtrim(ltrim(s));
}

// Inicializa la estructura
static void init_config(Config *cfg) {
    cfg->width = cfg->height = 0;

    cfg->monitor_capacity = INITIAL_MONITOR_CAP;
    cfg->monitor_count    = 0;
    cfg->monitors         = malloc(sizeof(char*) * cfg->monitor_capacity);

    cfg->shape_capacity = INITIAL_SHAPE_CAP;
    cfg->shape_count    = 0;
    cfg->shapes         = malloc(sizeof(ShapeConfig) * cfg->shape_capacity);
}

// Añade nueva ShapeConfig cuando se detecta una sección distinta a Canvas/Monitors:
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

// Parser manual de config.ini:
Config *load_config(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return NULL;

    Config *cfg = malloc(sizeof(Config));
    init_config(cfg);

    char line[LINE_MAX];
    char current_section[LINE_MAX] = "";
    ShapeConfig *cur_shape = NULL;

    while (fgets(line, LINE_MAX, f)) {
        char *s = trim(line);
        if (s[0] == '\0' || s[0] == ';' || s[0] == '#') continue;
        if (s[0] == '[') {
            char *end = strchr(s, ']');
            if (!end) continue;
            *end = '\0';
            strncpy(current_section, s+1, sizeof(current_section)-1);
            current_section[sizeof(current_section)-1] = '\0';
            if (strcmp(current_section, "Canvas") && strcmp(current_section, "Monitors")) {
                cur_shape = add_shape(cfg, current_section);
            }
        } else {
            char *eq = strchr(s, '=');
            if (!eq) continue;
            *eq = '\0';
            char *key = trim(s);
            char *val = trim(eq+1);

            if (!strcmp(current_section, "Canvas")) {
                if (!strcmp(key, "width"))  cfg->width  = atoi(val);
                if (!strcmp(key, "height")) cfg->height = atoi(val);
            }
            else if (!strcmp(current_section, "Monitors")) {
                if (!strcmp(key, "monitors")) {
                    char *tok = strtok(val, ",");
                    while (tok) {
                        char *m = trim(tok);
                        if (cfg->monitor_count >= cfg->monitor_capacity) {
                            cfg->monitor_capacity *= 2;
                            cfg->monitors = realloc(cfg->monitors,
                              sizeof(char*) * cfg->monitor_capacity);
                        }
                        cfg->monitors[cfg->monitor_count++] = strdup(m);
                        tok = strtok(NULL, ",");
                    }
                }
            }
            else if (cur_shape) {
                if (!strcmp(key, "shape_file"))    cur_shape->shape_file = strdup(val);
                else if (!strcmp(key, "x_start"))  cur_shape->x_start    = atoi(val);
                else if (!strcmp(key, "y_start"))  cur_shape->y_start    = atoi(val);
                else if (!strcmp(key, "x_end"))    cur_shape->x_end      = atoi(val);
                else if (!strcmp(key, "y_end"))    cur_shape->y_end      = atoi(val);
                else if (!strcmp(key, "rotation")) cur_shape->rotation   = atoi(val);
                else if (!strcmp(key, "start_time") || !strcmp(key, "star_time"))
                                                    cur_shape->start_time = atoi(val);
                else if (!strcmp(key, "end_time")) cur_shape->end_time   = atoi(val);
                else if (!strcmp(key, "scheduler")) strncpy(cur_shape->scheduler, val,
                                               sizeof(cur_shape->scheduler)-1);
                else if (!strcmp(key, "tickets"))   cur_shape->tickets    = atoi(val);
                else if (!strcmp(key, "deadline"))  cur_shape->deadline   = atoi(val);
            }
        }
    }
    fclose(f);
    return cfg;
}


int main(int argc, char **argv) {

    return 0;
}

