#include "../include/parser.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>


/**
 * QuitarEspacios
 *
 * Elimina espacios en blanco al inicio y al final de la cadena apuntada por 'inicio'.
 *
 * Entradas:
 *   inicio – puntero a la cadena que puede contener espacios en blanco al inicio o final.
 *
 * Retorna:
 *   char* – puntero al primer carácter no-espacio de la cadena original, con un '\0' colocado
 *           justo después del último carácter no-espacio. Si toda la cadena era espacios,
 *           retorna el puntero al '\0' final.
 */
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


/**
 * config_create
 *
 * Reserva e inicializa un nuevo Parser en memoria. Establece valores por defecto para
 * width, height, monitor_capacity, shape_capacity, y crea los arreglos iniciales para monitores
 * y formas.
 *
 * Entradas:
 *   ninguna
 *
 * Retorna:
 *   Parser* – puntero a la estructura Parser recién creada con campos inicializados.
 *             Si falla malloc, el comportamiento es indefinido.
 */
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

/**
 * config_destroy
 *
 * Libera toda la memoria asociada a un Parser: libera cada cadena de monitores, el arreglo
 * de monitores, y finalmente la estructura Parser misma.
 *
 * Entradas:
 *   cfg – puntero a la estructura Parser a liberar.
 *
 * Retorna:
 *   void
 */
void config_destroy(Parser *cfg) {
    for (int i = 0; i < cfg->monitor_count; i++)
        free(cfg->monitors[i]);
    free(cfg->monitors);
    free(cfg);
}


/**
 * add_shape
 *
 * Agrega una nueva entrada ShapeConfig al arreglo dinámico de shapes dentro de Parser.
 * Si el arreglo está lleno, se duplica su capacidad reallocando memoria. Inicializa
 * el ShapeConfig a cero y establece su campo 'name' como una copia de la sección dada.
 *
 * Entradas:
 *   cfg     – puntero a Parser que contiene el arreglo de ShapeConfig.
 *   section – cadena con el nombre de la sección (usada para strdup en el campo name).
 *
 * Retorna:
 *   ShapeConfig* – puntero a la nueva entrada ShapeConfig dentro del arreglo 'cfg->shapes'.
 */
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

/**
 * ltrim
 *
 * Elimina espacios en blanco al inicio de la cadena 's'. No modifica espacios finales.
 *
 * Entradas:
 *   s – cadena a la que se le eliminarán los espacios en blanco a la izquierda.
 *
 * Retorna:
 *   char* – puntero a la primera posición no-espacio dentro de 's'.
 */
static char *ltrim(char *s) { while (isspace((unsigned char)*s)) s++; return s; }

/**
 * rtrim
 *
 * Elimina espacios en blanco al final de la cadena 's'. No modifica espacios iniciales.
 *
 * Entradas:
 *   s – cadena a la que se le eliminarán los espacios en blanco a la derecha.
 *
 * Retorna:
 *   char* – mismo puntero 's', con todos los espacios finales convertidos en '\0'.
 */
static char *rtrim(char *s) { char *e = s + strlen(s) - 1; while (e >= s && isspace((unsigned char)*e)) *e-- = '\0'; return s; }

/**
 * trim
 *
 * Elimina espacios en blanco al inicio y al final de la cadena 's'.
 *
 * Entradas:
 *   s – cadena a la que se le eliminarán los espacios en blanco en ambos extremos.
 *
 * Retorna:
 *   char* – puntero al primer carácter no-espacio (después de ltrim), y con '\0' colocado
 *           tras el último carácter no-espacio (por efecto de rtrim).
 */
static char *trim(char *s)  { return rtrim(ltrim(s)); }


/**
 * load_config
 *
 * Carga un archivo de configuración en formato INI y lo parsea para llenar un Parser.
 * Crea secciones para Canvas, Monitors y cada forma definida. Cada sección puede contener
 * múltiples claves y valores. Los valores se convierten a tipos adecuados (int, char*).
 *
 * Entradas:
 *   filename – nombre del archivo de configuración a cargar.
 *
 * Retorna:
 *   Parser* – puntero al Parser lleno con la configuración del archivo. Si falla al abrir
 *             el archivo, retorna NULL.
 */
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


/**
 * load_shapes_content
 *
 * Lee cada archivo de forma referenciado en 'cfg->shapes[i].shape_file' y carga sus líneas
 * en memoria dentro de 'sh->shape_lines'. Si el número de líneas excede la capacidad actual,
 * duplica el arreglo usando realloc. Cada línea se guarda sin el carácter '\n'.
 *
 * Entradas:
 *   cfg – puntero al Parser que contiene al menos 'shape_count' ShapeConfig con 'shape_file' válido.
 *
 * Retorna:
 *   void
 */
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