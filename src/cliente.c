#define _POSIX_C_SOURCE 200809L

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <ncurses.h>


#define CMD_BUFLEN 128



/**
 * main
 *
 * Inicia la conexión con un servidor remoto para recibir comandos de dibujo y
 * los despliega en una ventana ncurses con la región asignada. El cliente:
 *   1) Valida argumentos (IP del servidor y puerto).
 *   2) Crea un socket TCP y se conecta al servidor.
 *   3) Espera a recibir la primera línea "REGION x_off w h" para calcular
 *      dimensiones y desplazamiento local.
 *   4) Inicializa ncurses y crea una ventana de tamaño h×w para dibujar,
 *      aplicando un borde (box).
 *   5) En un bucle infinito, lee líneas de comandos del servidor:
 *        - "CLEAR": borra la ventana y vuelve a dibujar el borde.
 *        - "DRAW gx gy ch color_pair": traduce coordenadas globales (gx, gy)
 *           a coordenadas locales (restando x_off) y dibuja el carácter 'ch'
 *           con el par de colores indicado.
 *           Si 'ch' == 'a', dibuja un espacio coloreado (para borrar píxeles).
 *        - "REFRESH": refresca la ventana para mostrar cambios acumulados.
 *        - "END": muestra un mensaje de finalización, espera una tecla y sale.
 *      Cada línea se procesa cuando se detecta '\n'; caracteres intermedios
 *      se acumulan en `line_buf`.
 *   6) Al desconectarse el servidor o recibir "END", cierra ncurses y termina.
 *
 * Entradas:
 *   argc, argv:
 *     argc debe ser 3 (nombre_del_programa, IP_Servidor, Puerto);
 *     argv[1] = dirección IP del servidor (formato IPv4).
 *     argv[2] = número de puerto (cadena que se convierte a int).
 *
 * Retorna:
 *   int – 0 si la ejecución finaliza correctamente, 1 en caso de error:
 *         - Error al crear socket, convertir IP, conectar al servidor.
 *         - No recepción de "REGION" o mal formato de la misma.
 *         - Error al crear ventana ncurses.
 */
int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "Uso: %s <IP_Servidor> <Puerto>\n", argv[0]);
        return 1;
    }
    const char *server_ip = argv[1];
    int port = atoi(argv[2]);


    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        perror("socket");
        return 1;
    }


    struct sockaddr_in serv = {
        .sin_family = AF_INET,
        .sin_port   = htons(port),
    };
    if (inet_pton(AF_INET, server_ip, &serv.sin_addr) <= 0) {
        perror("inet_pton");
        close(sock);
        return 1;
    }


    if (connect(sock, (struct sockaddr*)&serv, sizeof(serv)) < 0) {
        perror("connect");
        close(sock);
        return 1;
    }

    printf("Conectado al servidor %s:%d\n", server_ip, port);


    char buf[CMD_BUFLEN];
    ssize_t len = recv(sock, buf, sizeof(buf)-1, 0);
    if (len <= 0) {
        fprintf(stderr, "Error: no llegó REGION del servidor\n");
        close(sock);
        return 1;
    }
    buf[len] = '\0';

    int x_off, w, h;
    if (sscanf(buf, "REGION %d %d %d", &x_off, &w, &h) != 3) {
        fprintf(stderr, "Formato REGION inesperado: %s\n", buf);
        close(sock);
        return 1;
    }


    initscr();
    cbreak();
    noecho();
    curs_set(0);

    start_color();

    init_pair(1, COLOR_RED,    COLOR_BLACK);
    init_pair(2, COLOR_GREEN,  COLOR_BLACK);
    init_pair(3, COLOR_YELLOW, COLOR_BLACK);
    init_pair(4, COLOR_BLUE,   COLOR_BLACK);
    init_pair(5, COLOR_MAGENTA,COLOR_BLACK);
    init_pair(6, COLOR_CYAN,   COLOR_BLACK);
    init_pair(7, COLOR_WHITE,  COLOR_BLACK);



    init_pair(10, COLOR_CYAN, COLOR_BLACK);
    WINDOW *win = newwin(h, w, 0, 0);
    if (!win) {
        endwin();
        fprintf(stderr, "Error al crear ventana ncurses\n");
        close(sock);
        return 1;
    }
    wattron(win, COLOR_PAIR(10));
    box(win, 0, 0);
    wattroff(win, COLOR_PAIR(10));
    wrefresh(win);


    char recv_buf[512];
    char line_buf[CMD_BUFLEN];
    int line_len = 0;

    while (1) {
        ssize_t r = recv(sock, recv_buf, sizeof(recv_buf)-1, 0);
        if (r <= 0) {

            mvwprintw(win, 1, 1, "Desconectado del servidor.");
            wrefresh(win);
            break;
        }
        recv_buf[r] = '\0';


        for (int i = 0; i < r; i++) {
            char c = recv_buf[i];
            if (c == '\n') {

                line_buf[line_len] = '\0';


                char *cmd = line_buf;
                while (*cmd == ' ' || *cmd == '>') {
                    cmd++;
                }
                while (*cmd && isspace((unsigned char)*cmd)) {
                    cmd++;
                }


                if (strcmp(cmd, "CLEAR") == 0) {
                    werase(win);
                    box(win, 0, 0);
                    wrefresh(win);
                }
                else if (strncmp(cmd, "DRAW", 4) == 0) {
                    int gx, gy, color_pair;
                    char ch;
                    if (sscanf(cmd + 5, "%d %d %c %d", &gx, &gy, &ch, &color_pair) == 4) {
                        int local_x = gx - x_off;
                        int local_y = gy;

                        if (local_x >= 0 && local_x < w - 2 &&
                            local_y >= 0 && local_y < h - 2) {

                            if (ch == 'a') {
                                wattron(win, COLOR_PAIR(color_pair));
                                mvwaddch(win, local_y + 1, local_x + 1, ' ');
                                wattroff(win, COLOR_PAIR(color_pair));
                            }
                            else {
                                wattron(win, COLOR_PAIR(color_pair));
                                mvwaddch(win, local_y + 1, local_x + 1, ch);
                                wattroff(win, COLOR_PAIR(color_pair));
                            }


                        }
                    }
                }
                else if (strcmp(cmd, "REFRESH") == 0) {
                    wrefresh(win);

                }
                else if (strcmp(cmd, "END") == 0) {
                    mvwprintw(win, h/2, 2, "Animación finalizada. Presiona una tecla.");
                    wrefresh(win);
                    getch();
                    line_len = 0;
                    goto fin_while;
                }
                line_len = 0;
            }
            else {
                if (line_len < CMD_BUFLEN - 1) {
                    line_buf[line_len++] = c;
                }
            }
        }
    }

    fin_while:

        delwin(win);
    endwin();
    close(sock);
    return 0;
}