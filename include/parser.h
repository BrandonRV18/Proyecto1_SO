#ifndef PARSER_H
#define PARSER_H


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
    long long start_ms;
} ShapeConfig;


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