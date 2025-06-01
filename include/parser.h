#ifndef PARSER_H
#define PARSER_H


/**
 * ShapeConfig
 *
 * Almacena la configuración de una forma ASCII para animación:
 *   - name: nombre de la sección (identificador de la forma).
 *   - shape_file: ruta al archivo que contiene la forma en texto ASCII.
 *   - shape_lines: arreglo dinámico de cadenas que representan cada línea de la forma.
 *   - line_count: número de líneas actualmente cargadas en shape_lines.
 *   - line_capacity: capacidad actual (tamaño del arreglo) de shape_lines.
 *   - x_start, y_start: coordenadas globales donde inicia la animación de la forma.
 *   - x_end, y_end: coordenadas globales donde termina la animación de la forma.
 *   - rotation: ángulo de rotación que se aplica en cada paso (múltiplo de 90°).
 *   - start_time, end_time: tiempos (en milisegundos) relativos al inicio global para
 *                          comenzar y detener la animación.
 *   - tickets: número de “tickets” asignados para planificador Lottery (si aplica).
 *   - color_pair: índice de par de colores ncurses para dibujar la forma.
 *   - tid: identificador de hilo asignado (se inicializa cuando se crea el hilo).
 *   - start_ms: instante (timestamp en ms) en que se creó o programó el hilo;
 *               se usa para cómputos de temporización interna.
 */
typedef struct {
    char *name;
    char *shape_file;
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
    long long start_ms;
} ShapeConfig;


/**
 * Parser
 *
 * Contiene información de configuración leída desde el archivo INI:
 *   - width, height: dimensiones globales del canvas virtual (ancho y alto).
 *   - monitors: arreglo dinámico de cadenas con las direcciones/IPs de monitores.
 *   - monitor_count: número de monitores actualmente en la lista.
 *   - monitor_capacity: capacidad (tamaño del arreglo) reservada para monitores.
 *   - shapes: arreglo dinámico de ShapeConfig, una entrada por cada sección de forma.
 *   - shape_count: número de ShapeConfig actualmente cargados.
 *   - shape_capacity: capacidad del arreglo shapes.
 */
typedef struct {
    int width;
    int height;
    char **monitors;
    int monitor_count;
    int monitor_capacity;
    ShapeConfig *shapes;
    int shape_count, shape_capacity;
} Parser;


// Crea un Config inicializado con valores por defecto
Parser* config_create(void);

// Libera toda la memoria de Config
void config_destroy(Parser *cfg);

// Carga desde un INI muy simple (no maneja comentarios ni continuation)
Parser* load_config(const char *filename);

void load_shapes_content(Parser *cfg);

#endif