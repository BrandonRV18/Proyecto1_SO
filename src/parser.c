#include "../include/parser.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

static char* QuitarEspacios(char *inicio) {

    while (isspace((unsigned char)*inicio)) {
        inicio++;
    }

    if (*inicio == 0) {
        return inicio;
    }
    char *final = inicio + strlen(inicio) - 1;
    while (final > inicio && isspace((unsigned char)*final)) {
        final--;
    }
    final[1] = '\0';
    return inicio;
}


Parser* config_create(void) {
    Parser *cfg = malloc(sizeof(*cfg));
    cfg->width = 0;
    cfg->height = 0;
    cfg->monitor_capacity = 4;
    cfg->monitor_count = 0;
    cfg->monitors = malloc(sizeof(char*) * cfg->monitor_capacity);
    cfg->shape_capacity = 4;
    cfg->shape_count = 0;
    cfg->shapes = malloc(sizeof(ShapeConfig) * cfg->shape_capacity);
    return cfg;
}

void config_destroy(Parser *cfg) {
    for (int i = 0; i < cfg->monitor_count; i++)
        free(cfg->monitors[i]);
    free(cfg->monitors);
    free(cfg);
}

static ShapeConfig *add_shape(Parser *cfg, const char *section) {
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
static char *ltrim(char *s) { while (isspace((unsigned char)*s)) s++; return s; }
static char *rtrim(char *s) { char *e = s + strlen(s) - 1; while (e >= s && isspace((unsigned char)*e)) *e-- = '\0'; return s; }
static char *trim(char *s)  { return rtrim(ltrim(s)); }

Parser* load_config(const char *filename) {
    FILE *f = fopen(filename, "r");
    if (!f) return NULL;

    Parser *cfg = config_create();
    char linea[256], seccion[64] = "";
    ShapeConfig *cur_shape = NULL;

    while (fgets(linea, sizeof(linea), f)) {
        char *incio = QuitarEspacios(linea);
        if (*incio == '\0' || incio[0] == ';' || incio[0] == '#') {

            continue;
        }

        if (incio[0] == '[') {

            char *final = strchr(incio, ']');
            if (!final) continue;
            *final = '\0';
            strncpy(seccion, incio + 1, sizeof(seccion)-1);
            seccion[sizeof(seccion)-1] = '\0';


            if (strcmp(seccion, "Canvas") == 0) {
                cur_shape = NULL;
            }
            else if (strcmp(seccion, "Monitors") == 0) {
                cur_shape = NULL;
            }
            else {
                cur_shape = add_shape(cfg, seccion);
            }
        }
        else if (*incio && strchr(incio, '=')) {

            char *signoIgual = strchr(incio, '=');
            *signoIgual = '\0';
            char *llave  = QuitarEspacios(incio);
            char *valor  = QuitarEspacios(signoIgual + 1);

            if (strcmp(seccion, "Canvas") == 0) {

                if (strcmp(llave, "width") == 0) {
                    cfg->width = atoi(valor);
                }
                else if (strcmp(llave, "height") == 0) {
                    cfg->height = atoi(valor);
                }
            }
            else if (strcmp(seccion, "Monitors") == 0) {

                if (strcmp(llave, "monitors") == 0) {
                    char *token = strtok(valor, ",");
                    while (token) {
                        if (cfg->monitor_count >= cfg->monitor_capacity) {
                            cfg->monitor_capacity *= 2;
                            cfg->monitors = realloc(
                                cfg->monitors,
                                sizeof(char*) * cfg->monitor_capacity
                            );
                        }
                        cfg->monitors[cfg->monitor_count++] = strdup(trim(token));
                        token = strtok(NULL, ",");
                    }
                }
            }
            else if (cur_shape) {

                if (strcmp(llave, "shape_file") == 0) {
                    char *p = malloc((strlen(valor) + 1) * sizeof(char));
                    cur_shape->shape_file = p;

                    if (cur_shape->shape_file != NULL) {
                        strcpy(cur_shape->shape_file, valor);
                    } else {
                        perror("malloc para shape_file");
                    }

                }
                else if (strcmp(llave, "x_start") == 0) {
                    cur_shape->x_start = atoi(valor);
                }
                else if (strcmp(llave, "y_start") == 0) {
                    cur_shape->y_start = atoi(valor);
                }
                else if (strcmp(llave, "x_end") == 0) {
                    cur_shape->x_end = atoi(valor);
                }
                else if (strcmp(llave, "y_end") == 0) {
                    cur_shape->y_end = atoi(valor);
                }
                else if (strcmp(llave, "rotation") == 0) {
                    cur_shape->rotation = atoi(valor);
                }
                else if (strcmp(llave, "start_time") == 0) {
                    cur_shape->start_time = atoi(valor);
                }
                else if (strcmp(llave, "end_time") == 0) {
                    cur_shape->end_time = atoi(valor);
                }
                else if (strcmp(llave, "tickets") == 0) {
                    cur_shape->tickets = atoi(valor);
                }
            }

        }
    }

    fclose(f);
    return cfg;
}

void load_shapes_content(Parser *cfg) {

    for (int i = 0; i < cfg->shape_count; i++) {
        ShapeConfig *sh = &cfg->shapes[i];

        FILE *f = fopen(sh->shape_file, "r");

        if (!f) {
            fprintf(stderr, "No se pudo cargar la forma\n");
            continue;
        }

        sh->line_capacity = 16;
        sh->line_count = 0;
        sh->shape_lines = malloc(sizeof(char*) * sh->line_capacity);

        char buf[256];
        while (fgets(buf, 256, f)) {

            if (sh->line_count >= sh->line_capacity) {

                sh->line_capacity *= 2;
                sh->shape_lines = realloc(sh->shape_lines,
                                          sizeof(char*) * sh->line_capacity);

            }
            buf[strcspn(buf, "\n")] = '\0';
            size_t len = strlen(buf) + 1;


            char *copy = malloc(len);
            if (!copy) {
                perror("[ERROR] malloc para shape_lines");
                continue;
            }


            memcpy(copy, buf, len);


            sh->shape_lines[sh->line_count++] = copy;

        }

        fclose(f);

    }
}