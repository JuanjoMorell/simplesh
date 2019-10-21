/*
 * Shell `simplesh` (basado en el shell de xv6)
 *
 * Ampliación de Sistemas Operativos
 * Departamento de Ingeniería y Tecnología de Computadores
 * Facultad de Informática de la Universidad de Murcia
 *
 * Alumnos: MORELL FERNÁNDEZ, JUAN JOSÉ       (G1.1)
 *          PASTOR VALERA,    JOSÉ ANTONIO    (G1.1)
 *
 * Convocatoria: FEBRERO
 */


/*
 * Ficheros de cabecera
 */


#define _POSIX_C_SOURCE 200809L /* IEEE 1003.1-2008 (véase /usr/include/features.h) */
//#define NDEBUG                /* Traduce asertos y DMACROS a 'no ops' */

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include <sys/types.h>
#include <pwd.h>
#include <limits.h>
#include <libgen.h>

// Biblioteca readline
#include <readline/readline.h>
#include <readline/history.h>


/******************************************************************************
 * Constantes, macros y variables globales
 ******************************************************************************/


static const char* VERSION = "0.19";

// Niveles de depuración
#define DBG_CMD   (1 << 0)
#define DBG_TRACE (1 << 1)
// . . .
static int g_dbg_level = 0;

#ifndef NDEBUG
#define DPRINTF(dbg_level, fmt, ...)                            \
    do {                                                        \
        if (dbg_level & g_dbg_level)                            \
            fprintf(stderr, "%s:%d:%s(): " fmt,                 \
                    __FILE__, __LINE__, __func__, ##__VA_ARGS__);       \
    } while ( 0 )

#define DBLOCK(dbg_level, block)                                \
    do {                                                        \
        if (dbg_level & g_dbg_level)                            \
            block;                                              \
    } while( 0 );
#else
#define DPRINTF(dbg_level, fmt, ...)
#define DBLOCK(dbg_level, block)
#endif

#define TRY(x)                                                  \
    do {                                                        \
        int __rc = (x);                                         \
        if( __rc < 0 ) {                                        \
            fprintf(stderr, "%s:%d:%s: TRY(%s) failed\n",       \
                    __FILE__, __LINE__, __func__, #x);          \
            fprintf(stderr, "ERROR: rc=%d errno=%d (%s)\n",     \
                    __rc, errno, strerror(errno));              \
            exit(EXIT_FAILURE);                                 \
        }                                                       \
    } while( 0 )


// Número máximo de argumentos de un comando
#define MAX_ARGS 16
// Número de comandos internos
#define NUM_INTERNAL_CMDS 5
// Número de pids en segundo plano máximo
#define NUM_BG_PIDS 8

// Delimitadores
static const char WHITESPACE[] = " \t\r\n\v";
// Caracteres especiales
static const char SYMBOLS[] = "<|>&;()";
//Comando internos
static const char *INTERNAL_COMMANDS[] = {"exit", "cwd", "cd", "psplit", "bjobs"};
//PID en segundo plano
int BG_PIDS[NUM_BG_PIDS] = {0};

/******************************************************************************
 * Funciones auxiliares
 ******************************************************************************/


// Imprime el mensaje
void info(const char *fmt, ...)
{
    va_list arg;

    fprintf(stdout, "%s: ", __FILE__);
    va_start(arg, fmt);
    vfprintf(stdout, fmt, arg);
    va_end(arg);
}


// Imprime el mensaje de error
void error(const char *fmt, ...)
{
    va_list arg;

    fprintf(stderr, "%s: ", __FILE__);
    va_start(arg, fmt);
    vfprintf(stderr, fmt, arg);
    va_end(arg);
}


// Imprime el mensaje de error y aborta la ejecución
void panic(const char *fmt, ...)
{
    va_list arg;

    fprintf(stderr, "%s: ", __FILE__);
    va_start(arg, fmt);
    vfprintf(stderr, fmt, arg);
    va_end(arg);

    exit(EXIT_FAILURE);
}


// `fork()` que muestra un mensaje de error si no se puede crear el hijo
int fork_or_panic(const char* s)
{
    int pid;

    pid = fork();
    if(pid == -1)
        panic("%s failed: errno %d (%s)", s, errno, strerror(errno));
    return pid;
}


/******************************************************************************
 * Estructuras de datos `cmd`
 ******************************************************************************/


// Las estructuras `cmd` se utilizan para almacenar información que servirá a
// simplesh para ejecutar líneas de órdenes con redirecciones, tuberías, listas
// de comandos y tareas en segundo plano. El formato es el siguiente:

//     |----------+--------------+--------------|
//     | (1 byte) | ...          | ...          |
//     |----------+--------------+--------------|
//     | type     | otros campos | otros campos |
//     |----------+--------------+--------------|

// Nótese cómo las estructuras `cmd` comparten el primer campo `type` para
// identificar su tipo. A partir de él se obtiene un tipo derivado a través de
// *casting* forzado de tipo. Se consigue así polimorfismo básico en C.

// Valores del campo `type` de las estructuras de datos `cmd`
enum cmd_type { EXEC=1, REDR=2, PIPE=3, LIST=4, BACK=5, SUBS=6, INV=7 };

struct cmd { enum cmd_type type; };
//Variable global de cmd
struct cmd* cmd;

// Comando con sus parámetros
struct execcmd {
    enum cmd_type type;
    char* argv[MAX_ARGS];
    char* eargv[MAX_ARGS];
    int argc;
};

// Comando con redirección
struct redrcmd {
    enum cmd_type type;
    struct cmd* cmd;
    char* file;
    char* efile;
    int flags;
    mode_t mode;
    int fd;
};

// Comandos con tubería
struct pipecmd {
    enum cmd_type type;
    struct cmd* left;
    struct cmd* right;
};

// Lista de órdenes
struct listcmd {
    enum cmd_type type;
    struct cmd* left;
    struct cmd* right;
};

// Tarea en segundo plano (background) con `&`
struct backcmd {
    enum cmd_type type;
    struct cmd* cmd;
};

// Subshell
struct subscmd {
    enum cmd_type type;
    struct cmd* cmd;
};


/******************************************************************************
 * Funciones para construir las estructuras de datos `cmd`
 ******************************************************************************/


// Construye una estructura `cmd` de tipo `EXEC`
struct cmd* execcmd(void)
{
    struct execcmd* cmd;

    if ((cmd = malloc(sizeof(*cmd))) == NULL)
    {
        perror("execcmd: malloc");
        exit(EXIT_FAILURE);
    }
    memset(cmd, 0, sizeof(*cmd));
    cmd->type = EXEC;

    return (struct cmd*) cmd;
}

// Construye una estructura `cmd` de tipo `REDR`
struct cmd* redrcmd(struct cmd* subcmd,
        char* file, char* efile,
        int flags, mode_t mode, int fd)
{
    struct redrcmd* cmd;

    if ((cmd = malloc(sizeof(*cmd))) == NULL)
    {
        perror("redrcmd: malloc");
        exit(EXIT_FAILURE);
    }
    memset(cmd, 0, sizeof(*cmd));
    cmd->type = REDR;
    cmd->cmd = subcmd;
    cmd->file = file;
    cmd->efile = efile;
    cmd->flags = flags;
    cmd->mode = mode;
    cmd->fd = fd;

    return (struct cmd*) cmd;
}

// Construye una estructura `cmd` de tipo `PIPE`
struct cmd* pipecmd(struct cmd* left, struct cmd* right)
{
    struct pipecmd* cmd;

    if ((cmd = malloc(sizeof(*cmd))) == NULL)
    {
        perror("pipecmd: malloc");
        exit(EXIT_FAILURE);
    }
    memset(cmd, 0, sizeof(*cmd));
    cmd->type = PIPE;
    cmd->left = left;
    cmd->right = right;

    return (struct cmd*) cmd;
}

// Construye una estructura `cmd` de tipo `LIST`
struct cmd* listcmd(struct cmd* left, struct cmd* right)
{
    struct listcmd* cmd;

    if ((cmd = malloc(sizeof(*cmd))) == NULL)
    {
        perror("listcmd: malloc");
        exit(EXIT_FAILURE);
    }
    memset(cmd, 0, sizeof(*cmd));
    cmd->type = LIST;
    cmd->left = left;
    cmd->right = right;

    return (struct cmd*)cmd;
}

// Construye una estructura `cmd` de tipo `BACK`
struct cmd* backcmd(struct cmd* subcmd)
{
    struct backcmd* cmd;

    if ((cmd = malloc(sizeof(*cmd))) == NULL)
    {
        perror("backcmd: malloc");
        exit(EXIT_FAILURE);
    }
    memset(cmd, 0, sizeof(*cmd));
    cmd->type = BACK;
    cmd->cmd = subcmd;

    return (struct cmd*)cmd;
}

// Construye una estructura `cmd` de tipo `SUB`
struct cmd* subscmd(struct cmd* subcmd)
{
    struct subscmd* cmd;

    if ((cmd = malloc(sizeof(*cmd))) == NULL)
    {
        perror("subscmd: malloc");
        exit(EXIT_FAILURE);
    }

    memset(cmd, 0, sizeof(*cmd));
    cmd->type = SUBS;
    cmd->cmd = subcmd;

    return (struct cmd*) cmd;
}


/******************************************************************************
 * Funciones para realizar el análisis sintáctico de la línea de órdenes
 ******************************************************************************/


// `get_token` recibe un puntero al principio de una cadena (`start_of_str`),
// otro puntero al final de esa cadena (`end_of_str`) y, opcionalmente, dos
// punteros para guardar el principio y el final del token, respectivamente.
//
// `get_token` devuelve un *token* de la cadena de entrada.

int get_token(char** start_of_str, char const* end_of_str,
        char** start_of_token, char** end_of_token)
{
    char* s;
    int ret;

    // Salta los espacios en blanco
    s = *start_of_str;
    while (s < end_of_str && strchr(WHITESPACE, *s))
        s++;

    // `start_of_token` apunta al principio del argumento (si no es NULL)
    if (start_of_token)
        *start_of_token = s;

    ret = *s;
    switch (*s)
    {
        case 0:
        break;
        case '|':
        case '(':
        case ')':
        case ';':
        case '&':
        case '<':
            s++;
            break;
        case '>':
            s++;
            if (*s == '>')
            {
                ret = '+';
                s++;
            }
            break;

        default:

            // El caso por defecto (cuando no hay caracteres especiales) es el
            // de un argumento de un comando. `get_token` devuelve el valor
            // `'a'`, `start_of_token` apunta al argumento (si no es `NULL`),
            // `end_of_token` apunta al final del argumento (si no es `NULL`) y
            // `start_of_str` avanza hasta que salta todos los espacios
            // *después* del argumento. Por ejemplo:
            //
            //     |-----------+---+---+---+---+---+---+---+---+---+-----------|
            //     | (espacio) | a | r | g | u | m | e | n | t | o | (espacio)
            //     |
            //     |-----------+---+---+---+---+---+---+---+---+---+-----------|
            //                   ^                                   ^
            //            start_o|f_token                       end_o|f_token

            ret = 'a';
            while (s < end_of_str &&
                    !strchr(WHITESPACE, *s) &&
                    !strchr(SYMBOLS, *s))
                s++;
            break;
    }

    // `end_of_token` apunta al final del argumento (si no es `NULL`)
    if (end_of_token)
        *end_of_token = s;

    // Salta los espacios en blanco
    while (s < end_of_str && strchr(WHITESPACE, *s))
        s++;

    // Actualiza `start_of_str`
    *start_of_str = s;

    return ret;
}


// `peek` recibe un puntero al principio de una cadena (`start_of_str`), otro
// puntero al final de esa cadena (`end_of_str`) y un conjunto de caracteres
// (`delimiter`).
//
// El primer puntero pasado como parámero (`start_of_str`) avanza hasta el
// primer carácter que no está en el conjunto de caracteres `WHITESPACE`.
//
// `peek` devuelve un valor distinto de `NULL` si encuentra alguno de los
// caracteres en `delimiter` justo después de los caracteres en `WHITESPACE`.

int peek(char** start_of_str, char const* end_of_str, char* delimiter)
{
    char* s;

    s = *start_of_str;
    while (s < end_of_str && strchr(WHITESPACE, *s))
        s++;
    *start_of_str = s;

    return *s && strchr(delimiter, *s);
}


// Definiciones adelantadas de funciones
struct cmd* parse_line(char**, char*);
struct cmd* parse_pipe(char**, char*);
struct cmd* parse_exec(char**, char*);
struct cmd* parse_subs(char**, char*);
struct cmd* parse_redr(struct cmd*, char**, char*);
struct cmd* null_terminate(struct cmd*);


// `parse_cmd` realiza el *análisis sintáctico* de la línea de órdenes
// introducida por el usuario.
//
// `parse_cmd` utiliza `parse_line` para obtener una estructura `cmd`.

struct cmd* parse_cmd(char* start_of_str)
{
    char* end_of_str;
    struct cmd* cmd;

    DPRINTF(DBG_TRACE, "STR\n");

    end_of_str = start_of_str + strlen(start_of_str);

    cmd = parse_line(&start_of_str, end_of_str);

    // Comprueba que se ha alcanzado el final de la línea de órdenes
    peek(&start_of_str, end_of_str, "");
    if (start_of_str != end_of_str)
        error("%s: error sintáctico: %s\n", __func__);

    DPRINTF(DBG_TRACE, "END\n");

    return cmd;
}


// `parse_line` realiza el análisis sintáctico de la línea de órdenes
// introducida por el usuario.
//
// `parse_line` comprueba en primer lugar si la línea contiene alguna tubería.
// Para ello `parse_line` llama a `parse_pipe` que a su vez verifica si hay
// bloques de órdenes y/o redirecciones.  A continuación, `parse_line`
// comprueba si la ejecución de la línea se realiza en segundo plano (con `&`)
// o si la línea de órdenes contiene una lista de órdenes (con `;`).

struct cmd* parse_line(char** start_of_str, char* end_of_str)
{
    struct cmd* cmd;
    int delimiter;

    cmd = parse_pipe(start_of_str, end_of_str);

    while (peek(start_of_str, end_of_str, "&"))
    {
        // Consume el delimitador de tarea en segundo plano
        delimiter = get_token(start_of_str, end_of_str, 0, 0);
        assert(delimiter == '&');

        // Construye el `cmd` para la tarea en segundo plano
        cmd = backcmd(cmd);
    }

    if (peek(start_of_str, end_of_str, ";"))
    {
        if (cmd->type == EXEC && ((struct execcmd*) cmd)->argv[0] == 0)
            error("%s: error sintáctico: no se encontró comando\n", __func__);

        // Consume el delimitador de lista de órdenes
        delimiter = get_token(start_of_str, end_of_str, 0, 0);
        assert(delimiter == ';');

        // Construye el `cmd` para la lista
        cmd = listcmd(cmd, parse_line(start_of_str, end_of_str));
    }

    return cmd;
}


// `parse_pipe` realiza el análisis sintáctico de una tubería de manera
// recursiva si encuentra el delimitador de tuberías '|'.
//
// `parse_pipe` llama a `parse_exec` y `parse_pipe` de manera recursiva para
// realizar el análisis sintáctico de todos los componentes de la tubería.

struct cmd* parse_pipe(char** start_of_str, char* end_of_str)
{
    struct cmd* cmd;
    int delimiter;

    cmd = parse_exec(start_of_str, end_of_str);

    if (peek(start_of_str, end_of_str, "|"))
    {
        if (cmd->type == EXEC && ((struct execcmd*) cmd)->argv[0] == 0)
            error("%s: error sintáctico: no se encontró comando\n", __func__);

        // Consume el delimitador de tubería
        delimiter = get_token(start_of_str, end_of_str, 0, 0);
        assert(delimiter == '|');

        // Construye el `cmd` para la tubería
        cmd = pipecmd(cmd, parse_pipe(start_of_str, end_of_str));
    }

    return cmd;
}


// `parse_exec` realiza el análisis sintáctico de un comando a no ser que la
// expresión comience por un paréntesis, en cuyo caso se llama a `parse_subs`.
//
// `parse_exec` reconoce las redirecciones antes y después del comando.

struct cmd* parse_exec(char** start_of_str, char* end_of_str)
{
    char* start_of_token;
    char* end_of_token;
    int token, argc;
    struct execcmd* cmd;
    struct cmd* ret;

    // ¿Inicio de un bloque?
    if (peek(start_of_str, end_of_str, "("))
        return parse_subs(start_of_str, end_of_str);

    // Si no, lo primero que hay en una línea de órdenes es un comando

    // Construye el `cmd` para el comando
    ret = execcmd();
    cmd = (struct execcmd*) ret;

    // ¿Redirecciones antes del comando?
    ret = parse_redr(ret, start_of_str, end_of_str);

    // Bucle para separar los argumentos de las posibles redirecciones
    argc = 0;
    while (!peek(start_of_str, end_of_str, "|)&;"))
    {
        if ((token = get_token(start_of_str, end_of_str,
                        &start_of_token, &end_of_token)) == 0)
            break;

        // El siguiente token debe ser un argumento porque el bucle
        // para en los delimitadores
        if (token != 'a')
            error("%s: error sintáctico: se esperaba un argumento\n", __func__);

        // Almacena el siguiente argumento reconocido. El primero es
        // el comando
        cmd->argv[argc] = start_of_token;
        cmd->eargv[argc] = end_of_token;
        cmd->argc = ++argc;
        if (argc >= MAX_ARGS)
            panic("%s: demasiados argumentos\n", __func__);

        // ¿Redirecciones después del comando?
        ret = parse_redr(ret, start_of_str, end_of_str);
    }

    // El comando no tiene más parámetros
    cmd->argv[argc] = 0;
    cmd->eargv[argc] = 0;

    return ret;
}


// `parse_subs` realiza el análisis sintáctico de un bloque de órdenes
// delimitadas por paréntesis o `subshell` llamando a `parse_line`.
//
// `parse_subs` reconoce las redirecciones después del bloque de órdenes.

struct cmd* parse_subs(char** start_of_str, char* end_of_str)
{
    int delimiter;
    struct cmd* cmd;
    struct cmd* scmd;

    // Consume el paréntesis de apertura
    if (!peek(start_of_str, end_of_str, "("))
        error("%s: error sintáctico: se esperaba '('", __func__);
    delimiter = get_token(start_of_str, end_of_str, 0, 0);
    assert(delimiter == '(');

    // Realiza el análisis sintáctico hasta el paréntesis de cierre
    scmd = parse_line(start_of_str, end_of_str);

    // Construye el `cmd` para el bloque de órdenes
    cmd = subscmd(scmd);

    // Consume el paréntesis de cierre
    if (!peek(start_of_str, end_of_str, ")"))
        error("%s: error sintáctico: se esperaba ')'", __func__);
    delimiter = get_token(start_of_str, end_of_str, 0, 0);
    assert(delimiter == ')');

    // ¿Redirecciones después del bloque de órdenes?
    cmd = parse_redr(cmd, start_of_str, end_of_str);

    return cmd;
}


// `parse_redr` realiza el análisis sintáctico de órdenes con
// redirecciones si encuentra alguno de los delimitadores de
// redirección ('<' o '>').

struct cmd* parse_redr(struct cmd* cmd, char** start_of_str, char* end_of_str)
{
    int delimiter;
    char* start_of_token;
    char* end_of_token;

    // Si lo siguiente que hay a continuación es delimitador de
    // redirección...
    while (peek(start_of_str, end_of_str, "<>"))
    {
        // Consume el delimitador de redirección
        delimiter = get_token(start_of_str, end_of_str, 0, 0);
        assert(delimiter == '<' || delimiter == '>' || delimiter == '+');

        // El siguiente token tiene que ser el nombre del fichero de la
        // redirección entre `start_of_token` y `end_of_token`.
        if ('a' != get_token(start_of_str, end_of_str, &start_of_token, &end_of_token))
            error("%s: error sintáctico: se esperaba un fichero", __func__);

        // Construye el `cmd` para la redirección
        switch(delimiter)
        {
            case '<':
                cmd = redrcmd(cmd, start_of_token, end_of_token, O_RDONLY, 0, STDIN_FILENO);
                break;
            case '>':
                cmd = redrcmd(cmd, start_of_token, end_of_token, O_WRONLY|O_CREAT|O_TRUNC, S_IRWXU, STDOUT_FILENO);
                break;
            case '+': // >>
                cmd = redrcmd(cmd, start_of_token, end_of_token, O_WRONLY|O_CREAT|O_APPEND, S_IRWXU, STDOUT_FILENO);
                break;
        }
    }

    return cmd;
}


// Termina en NULL todas las cadenas de las estructuras `cmd`
struct cmd* null_terminate(struct cmd* cmd)
{
    struct execcmd* ecmd;
    struct redrcmd* rcmd;
    struct pipecmd* pcmd;
    struct listcmd* lcmd;
    struct backcmd* bcmd;
    struct subscmd* scmd;
    int i;

    if(cmd == 0)
        return 0;

    switch(cmd->type)
    {
        case EXEC:
            ecmd = (struct execcmd*) cmd;
            for(i = 0; ecmd->argv[i]; i++)
                *ecmd->eargv[i] = 0;
            break;

        case REDR:
            rcmd = (struct redrcmd*) cmd;
            null_terminate(rcmd->cmd);
            *rcmd->efile = 0;
            break;

        case PIPE:
            pcmd = (struct pipecmd*) cmd;
            null_terminate(pcmd->left);
            null_terminate(pcmd->right);
            break;

        case LIST:
            lcmd = (struct listcmd*) cmd;
            null_terminate(lcmd->left);
            null_terminate(lcmd->right);
            break;

        case BACK:
            bcmd = (struct backcmd*) cmd;
            null_terminate(bcmd->cmd);
            break;

        case SUBS:
            scmd = (struct subscmd*) cmd;
            null_terminate(scmd->cmd);
            break;

        case INV:
        default:
            panic("%s: estructura `cmd` desconocida\n", __func__);
    }

    return cmd;
}

/******************************************************************************
 * Free CMD
 ******************************************************************************/

void free_cmd(struct cmd* cmd)
{
    struct execcmd* ecmd;
    struct redrcmd* rcmd;
    struct listcmd* lcmd;
    struct pipecmd* pcmd;
    struct backcmd* bcmd;
    struct subscmd* scmd;

    if(cmd == 0) return;

    switch(cmd->type)
    {
        case EXEC:
            ecmd = (struct execcmd*) cmd;
            free(ecmd);
            break;

        case REDR:
            rcmd = (struct redrcmd*) cmd;
            free_cmd(rcmd->cmd);

            //free(rcmd->cmd);
            free(rcmd);
            break;

        case LIST:
            lcmd = (struct listcmd*) cmd;

            free_cmd(lcmd->left);
            free_cmd(lcmd->right);

            //free(lcmd->right);
            //free(lcmd->left);
            free(lcmd);
            break;

        case PIPE:
            pcmd = (struct pipecmd*) cmd;

            free_cmd(pcmd->left);
            free_cmd(pcmd->right);

            //free(pcmd->right);
            //free(pcmd->left);
            free(pcmd);
            break;

        case BACK:
            bcmd = (struct backcmd*) cmd;

            free_cmd(bcmd->cmd);

            //free(bcmd->cmd);
            free(bcmd);
            break;

        case SUBS:
            scmd = (struct subscmd*) cmd;

            free_cmd(scmd->cmd);

            //free(scmd->cmd);
            free(scmd);
            break;

        case INV:
        default:
            panic("%s: estructura `cmd` desconocida\n", __func__);
    }
    // free(cmd);
}


/******************************************************************************
 * Comandos internos de `simplesh`
 ******************************************************************************/

int is_internal_cmd(char* cmd_name)
{
    if (cmd_name == NULL) return 0;

    for(int i = 0; i < NUM_INTERNAL_CMDS; i++) {
        if(strcmp(INTERNAL_COMMANDS[i], cmd_name) == 0)
            return 1;
    }

    return 0;
}

void run_cwd()
{

    char path[PATH_MAX];
    if (!getcwd(path, PATH_MAX))
    {
        perror("getcwd");
        exit(EXIT_FAILURE);
    }

    printf("cwd: %s\n", path);
}

void run_exit() 
{ 
    free_cmd(cmd);
    exit(0); 
}

void run_cd(char* path)
{
    char cwd[PATH_MAX];
    if (getcwd(cwd, sizeof(cwd)) == NULL)
    {
        perror("getcwd");
        exit(EXIT_FAILURE);
    }

    // cd
	if (path == NULL)
    {
		char* home = getenv("HOME");
        if (home != NULL)
        {
            if (chdir(home) == -1)
            {
                //Cambiamos al directorio HOME
                perror("chdir");
                exit(EXIT_FAILURE);
            }
		    if (setenv("OLDPWD", cwd, 1) == -1)
            {
                //Actualizamos la variable de entorno OLDPWD
                perror("setenv");
                exit(EXIT_FAILURE);
            }
	    }
    }

    // cd [-]
    else if (strcmp(path, "-") == 0) 
    {
		char* oldpwd = getenv("OLDPWD");
        if (oldpwd == NULL) printf("run_cd: Variable OLDPWD no definida\n");
        else 
        {
		    if (chdir(oldpwd) == -1) 
            {
                perror("chdir");
                exit(EXIT_FAILURE);
            }
            if (setenv("OLDPWD", cwd, 1) == -1) 
            {
                perror("setenv");
                exit(EXIT_FAILURE);
            }
        }
	}

    // cd dir
    else {
        if (chdir(path) == -1) printf("run_cd: No existe el directorio '%s'\n", path);
        if(setenv("OLDPWD", cwd, 1) == -1) 
        {
            perror("setenv");
            exit(EXIT_FAILURE);
        }
	}
}

void escribir_bytes(int fd, char* file, int NBYTES, int BSIZE)
{
    char data[BSIZE]; // Data buffer

    int read_from_source    = 0; // Bytes leídos del fichero original
    int remaining           = 0; // Bytes restantes por escribir en el fichero
    int total_written       = 0; // Bytes totales escritos del buffer
    int is_incomplete       = 0; // Flag de fichero incompleto
    int incomplete_fd       = 0; // Descriptor de fichero incompleto
    int current_file        = 0; // Descriptor del fichero actual
    int next_file_id        = 0; // Siguiente número de fichero
    int bytes_in_buffer     = 0; // Bytes totales en el buffer

    // Leer mientras el fichero no esté vacío.
    while ((read_from_source = read(fd, data, BSIZE)) != 0)
    {
        bytes_in_buffer = read_from_source;
        // Actúa como puntero al buffer de bytes, cada vez que escribamos
        // bytes del buffer en el fichero, se sumará el número de bytes
        // escritos a esta variable, por lo que la próxima vez 
        // empezaremos a escribir en el fichero desde la posición
        // data + total_written
        total_written = 0;

        // Si el fichero está incompleto
        if (is_incomplete)
        {
            // Si faltan más bytes por escribir de los que hay en el buffer
            if (remaining > bytes_in_buffer)
            {
                total_written += write(incomplete_fd, data, bytes_in_buffer);
                remaining -= bytes_in_buffer;
                bytes_in_buffer = 0;
            }
            else
            {
                total_written += write(incomplete_fd, data, remaining);
                //fsync(incomplete_fd);
                is_incomplete = 0;
                bytes_in_buffer -= remaining;
                close(incomplete_fd);
            }
        }

        // Mientras queden bytes en el buffer...
        while (bytes_in_buffer > 0)
        {
            // Construimos el nombre del nuevo fichero
            char file_name[32];
            sprintf(file_name, "%s%d", file, next_file_id);
            next_file_id++;

            // Abrimos el fichero
            current_file = open(file_name, O_CREAT|O_RDWR|O_TRUNC, S_IRWXU);

            // Si el tamaño en bytes del fichero es mayor que los bytes
            // que tenemos actualmente en el buffer, el fichero quedará
            // incompleto y todos los bytes del buffer se consumirán
            if (NBYTES > bytes_in_buffer)
            {
                total_written += write(current_file, data + total_written, bytes_in_buffer);
                is_incomplete = 1;
                incomplete_fd = current_file;
                remaining = NBYTES - bytes_in_buffer;
                bytes_in_buffer = 0;
            }
            else
            {
                total_written += write(current_file, data + total_written, NBYTES);
                //fsync(current_file);
                bytes_in_buffer -= NBYTES;
                close(current_file);
            }
        }
    }
}

int comprobar_linea(char* DATOS, int escritos, int lineas, int bytes_leidos)
{
    int leidas = 0;
    int cont = -1;
    for(int i = escritos; i<bytes_leidos; i++)
    {
        if(DATOS[i] ==  '\n')
        {
            cont = (i-escritos)+1;
            leidas++;
        }
        if(leidas == lineas)
        {
            cont = (i-escritos)+1;
            break;
        }
    }
    return cont;
}

int contar_lineas(char* DATOS, int escritos, int bytes_escribir)
{
    int cont = 0;
    for (int i = escritos; i<escritos+bytes_escribir; i++) 
    {
        if(DATOS[i] == '\n')
        {
            cont++;
        }
    }
    return cont;
}

void escribir_lineas(int fd, char* file, int NLINES, int BSIZE) 
{
    char data[BSIZE]; // Data buffer

    int read_from_source    = 0; // Bytes leídos del fichero original
    int remaining           = NLINES; // Lineas restantes por escribir en el fichero
    int total_written       = 0; // Bytes totales escritos del buffer
    int is_incomplete       = 0; // Flag de fichero incompleto
    int incomplete_fd       = 0; // Descriptor de fichero incompleto
    int current_file        = 0; // Descriptor del fichero actual
    int next_file_id        = 0; // Siguiente número de fichero
    int bytes_in_buffer     = 0; // Bytes totales en el buffer

    // Leer mientras el fichero no esté vacío.
    while ((read_from_source = read(fd, data, BSIZE)) != 0)
    {
        bytes_in_buffer = read_from_source;
        // Actúa como puntero al buffer de bytes, cada vez que escribamos
        // bytes del buffer en el fichero, se sumará el número de bytes
        // escritos a esta variable, por lo que la próxima vez 
        // empezaremos a escribir en el fichero desde la posición
        // data + total_written
        total_written = 0;

        // Si el fichero está incompleto
        if (is_incomplete)
        {
            int comprobacion = comprobar_linea(data, total_written, remaining, bytes_in_buffer);
            int lineas_comprobadas = contar_lineas(data, total_written, comprobacion);
            if (comprobacion == -1 || lineas_comprobadas < remaining)
            {
                total_written += write(incomplete_fd, data, bytes_in_buffer);
                remaining -= lineas_comprobadas;
                bytes_in_buffer = 0;
            }
            else
            {
                total_written += write(incomplete_fd, data, comprobacion);
                fsync(incomplete_fd);
                close(incomplete_fd);
                is_incomplete = 0;
                remaining = NLINES;
                bytes_in_buffer -= comprobacion;
            }
        }

        // Mientras queden bytes en el buffer...
        while (bytes_in_buffer > 0)
        {
            // Construimos el nombre del nuevo fichero
            char file_name[32];
            sprintf(file_name, "%s%d", file, next_file_id);
            next_file_id++;

            // Abrimos el fichero
            current_file = open(file_name, O_CREAT|O_RDWR|O_TRUNC, S_IRWXU);

            int comprobacion = comprobar_linea(data, total_written, remaining, bytes_in_buffer);
            int lineas_comprobadas = contar_lineas(data, total_written, comprobacion);
            if (lineas_comprobadas < remaining)
            {
                total_written += write(current_file, data + total_written, bytes_in_buffer);
                is_incomplete = 1;
                incomplete_fd = current_file;
                remaining -= lineas_comprobadas;
                bytes_in_buffer = 0;
            }
            else
            {
                total_written += write(current_file, data + total_written, comprobacion);
                fsync(current_file);
                close(current_file);
                bytes_in_buffer -= comprobacion;
                remaining = NLINES;
            }
        }
    }
}

void run_psplit(struct execcmd* ecmd)
{
    int opt;
    optind = 1;

    /* Tamaño máximo del buffer */
    static int MAX_BSIZE = 1048576;

    /* Valores por defecto */
    int NLINES  = 0;
    int BSIZE   = 1024;
    int PROCS   = 1;
    int NBYTES  = 1024;
    
    while ((opt = getopt(ecmd->argc, ecmd->argv, "l:b:s:p:h")) != -1)
    {
        switch (opt)
        {
            case 'l': { NLINES = atoi(optarg); break; }
            case 'b': { NBYTES = atoi(optarg); break; }
            case 's': { BSIZE  = atoi(optarg); break; }
            case 'p': { PROCS  = atoi(optarg); break; }
            case 'h':
                printf("Uso: %s [-l NLINES] [-b NBYTES] [-s BSIZE] [-p PROCS] [FILE1] [FILE2]...\n", ecmd->argv[0]);
                printf("     Opciones:\n");
                printf("     -l NLINES Número máximo de líneas por fichero.\n");
                printf("     -b NBYTES Número máximo de bytes por fichero.\n");
                printf("     -s BSIZE  Tamaño en bytes de los bloques leídos de [FILEn] o stdin.\n");
                printf("     -p PROCS  Número máximo de procesos simultáneos.\n");
                printf("     -h        Ayuda\n\n");
                return;

            default:
                printf("Uso: %s [-l NLINES] [-b NBYTES] [-s BSIZE] [-p PROCS] [FILE1] [FILE2]...\n", ecmd->argv[0]);
                return;
        }   
    }

    if (NLINES != 0 && NBYTES != 1024)
    {
        printf("%s: Opciones incompatibles\n", ecmd->argv[0]);
        return;
    }

    if (BSIZE < 1 || BSIZE > MAX_BSIZE)
    {
        printf("%s: Opción -s no válida\n", ecmd->argv[0]);
        return;   
    }

    if (PROCS < 1)
    {
        printf("%s: Opción -p no válida\n", ecmd->argv[0]);
        return;
    }

    /*
     * fork(psplit(f1); fork(psplit(f2)); ... ; fork(psplit(fn)))
     * Hacer tantos fork como se pueda (depende de NPROCS)
     * Una vez que no se puedan procesar más ficheros pero aún queden
     * ficheros por procesar, habrá que esperar a que acabe el más
     * antigüo.
     *
     * Almacenar los PID de los procesos que se lanzan para saber si ha
     * finalizado el más antigüo.
     *
     * Si no quedan más ficheros se finaliza el bucle y se espera (wait())
     * a los procesos en vuelo.
     */

    int num_files = ecmd->argc - optind;
    char* file_names[num_files];

    /* Almacenar nombre de los ficheros. */
    for (int i = optind, index = 0; i < ecmd->argc; i++, index++)
        file_names[index] = ecmd->argv[i];

    // Si no hay ficheros, leer de la entrada estándar
    if (num_files == 0)
    {
        if (NBYTES != 1024) escribir_bytes(STDIN_FILENO, "stdin", NBYTES, BSIZE);
        else if (NLINES != 0) escribir_lineas(STDIN_FILENO, "stdin", NLINES, BSIZE);
        else escribir_bytes(STDIN_FILENO, "stdin", NBYTES, BSIZE);
    }
    else if (PROCS > 1)
    {
        int pid;                    /* PID del proceso hijo */
        int num_children = 0;       /* Contador de hijos creados */
        int procesos_en_vuelo = 0;  /* Número de procesos corriendo al mismo tiempo */
        int running_pids[PROCS];    /* PIDs de los procesos ejecutándose */
        int oldest_pid;             /* */
        int status;
        int procesos_terminados = 0;

        // Mientras no se hayan creado tantos procesos hijo como
        // ficheros hay que procesar...
        while (num_children < num_files)
        {
            // Si no se ha alcanzado PROCS, crear un nuevo proceso...
            while (procesos_en_vuelo < PROCS)
            {
                int pid = fork();
                // CHILD execution.
                if (pid == 0)
                {
                    int fd = open(file_names[num_children], O_RDONLY);
                    if (NBYTES != 1024) escribir_bytes(fd, file_names[num_children], NBYTES, BSIZE);
                    else if (NLINES != 0) escribir_lineas(fd, file_names[num_children], NLINES, BSIZE);
                    else escribir_bytes(fd, file_names[num_children], NBYTES, BSIZE);
                    exit(EXIT_SUCCESS);
                }
                // PARENT execution.
                // Almacenar PID del proceso creado. 
                for (int i = 0; i < PROCS; i++)
                    if (!running_pids[i]) { running_pids[i] = pid; break; }
                
                // Actualizar variables de control. 
                num_children++;
                procesos_en_vuelo++;
                
                if(procesos_en_vuelo == num_files) break;

            }
            
            // Esperar al proceso más antigüo...
            oldest_pid = running_pids[0];
            for(int i = 0; i < PROCS; i++) {
                if ( running_pids[i] < oldest_pid ) 
                {
                    oldest_pid = running_pids[i];
                }
            }

            int ret;
            if ((ret = waitpid(oldest_pid, &status, 0)) > 0)
            {
                for (int i = 0; i < PROCS; i++)
                {
                    if (ret == running_pids[i])
                    {
                        running_pids[i] = 0;
                        procesos_en_vuelo--;
                        procesos_terminados++;
                        break;
                    }
                }
            }
            
        }

        for(int i = 0; i < num_children-procesos_terminados; i++)
        {
            int hijo = waitpid(-1, &status, 0);
        }

    }
    else
    {
        for (int i = 0; i < num_files; i++)
        {
            int fd = open(file_names[i], O_RDONLY);
            if (NBYTES != 1024) escribir_bytes(fd, file_names[i], NBYTES, BSIZE);
            else if (NLINES != 0) escribir_lineas(fd, file_names[i], NLINES, BSIZE);
            else escribir_bytes(fd, file_names[i], NBYTES, BSIZE);
        }
    }
}

void run_bjobs(struct execcmd* ecmd)
{
    int opt;
    optind = 1;

    while ((opt = getopt(ecmd->argc, ecmd->argv, "kh")) != -1)
    {
        switch (opt)
        {
            case 'k':
                //Matamos a todos los procesos en segundo plano
                for(int i = 0; i<NUM_BG_PIDS; i++){
                if(BG_PIDS[i])
                    kill(BG_PIDS[i], SIGKILL);
                }
                return;
            case 'h':
                printf("Uso: %s [-k] [-h]\n", ecmd->argv[0]);
                printf("     Opciones:\n");
                printf("     -k Mata todos los procesos en segundo plano.\n");
                printf("     -h Ayuda\n");
                return;

            default:
                return;
        }   
    }   

    //Mostramos los procesos que estan en segundo plano
    for(int i = 0; i<NUM_BG_PIDS; i++){
        if(BG_PIDS[i])
            printf("[%d]\n", BG_PIDS[i]);
    }

}

void run_internal_cmd(struct execcmd* ecmd) 
{
	if (strcmp(ecmd->argv[0], "cwd") == 0)       run_cwd();
	else if (strcmp(ecmd->argv[0], "exit") == 0) run_exit();
	else if (strcmp(ecmd->argv[0], "cd") == 0)
	{
	    if (ecmd->argc > 2) printf("run_cd: Demasiados argumentos\n");
	    else run_cd(ecmd->argv[1]);
	}
    else if (strcmp(ecmd->argv[0], "psplit") == 0) run_psplit(ecmd);
    else if (strcmp(ecmd->argv[0], "bjobs") == 0) run_bjobs(ecmd);
}

/******************************************************************************
 * Funciones para la ejecución de la línea de órdenes
 ******************************************************************************/

void exec_cmd(struct execcmd* ecmd)
{
    assert(ecmd->type == EXEC);

    if (ecmd->argv[0] == NULL) exit(EXIT_SUCCESS);

    execvp(ecmd->argv[0], ecmd->argv);

    panic("no se encontró el comando '%s'\n", ecmd->argv[0]);
}

void handle_sigchld(int sig) 
{
    int saved_errno = errno;
    int pid;
    while ((pid = waitpid(-1, 0, WNOHANG)) > 0) 
    {
        for(int i = 0; i<NUM_BG_PIDS; i++)
        {
            if(BG_PIDS[i] == pid) 
            {
                BG_PIDS[i] = 0;

                //Imprimimos por STDOUT el pid que ha finalizado
                char buf[12];
                sprintf(buf, "[%d]", pid);
                write(STDOUT_FILENO, buf, strlen(buf));
                break;
            }
        }       
    }
    errno = saved_errno;
}

void block_sigchld()
{
    struct sigaction sa;
    memset(&sa.sa_flags, 0, sizeof(int));
    sa.sa_handler = SIG_DFL;

    if(sigaction(SIGCHLD, &sa, 0) == -1)
    {
        perror("sigaction");
        exit(EXIT_FAILURE);
    }
}

void unblock_sigchld()
{
    struct sigaction sa;
    memset(&sa.sa_flags, 0, sizeof(int));
    sa.sa_handler = &handle_sigchld;

    if(sigaction(SIGCHLD, &sa, 0) == -1)
    {
        perror("sigaction");
        exit(EXIT_FAILURE);
    }
}

void run_cmd(struct cmd* cmd)
{
    struct execcmd* ecmd;
    struct redrcmd* rcmd;
    struct listcmd* lcmd;
    struct pipecmd* pcmd;
    struct backcmd* bcmd;
    struct subscmd* scmd;
    int p[2];
    int fd;
    int pid, status;
    int pidD, pidI;
    sigset_t mask_all, mask_one, prev_one;

    DPRINTF(DBG_TRACE, "STR\n");

    if(cmd == 0) return;

    switch(cmd->type)
    {
        case EXEC:
            ecmd = (struct execcmd*) cmd;
	    	//Comprobacion de si es comando interno o externo
	    	if (is_internal_cmd(ecmd->argv[0]) == 1) {
	    		run_internal_cmd(ecmd);
	    	} else {
            	if ((pid = fork_or_panic("fork EXEC")) == 0)
                	exec_cmd(ecmd);
            	TRY( waitpid(pid, &status, 0) );
	    	}
            break;

        case REDR:
            rcmd = (struct redrcmd*) cmd;

            if(rcmd->cmd->type == EXEC)
            {
                ecmd = (struct execcmd*) rcmd->cmd;
                if(is_internal_cmd(ecmd->argv[0]) == 1)
                {
                    int stdout_bak = dup(rcmd->fd);
                    if ((fd = open(rcmd->file, rcmd->flags, rcmd->mode)) < 0)
                    {
                        perror("open");
                        exit(EXIT_FAILURE);
                    }
                    TRY( dup2(fd, rcmd->fd) );
                    TRY( close(fd) );
                    run_internal_cmd(ecmd);
                    TRY( dup2(stdout_bak, rcmd->fd) );
                    TRY( close(stdout_bak) );
                    break;
                }
            }
            if ((pid = fork_or_panic("fork REDR")) == 0)
            {
                TRY( close(rcmd->fd) );
                if ((fd = open(rcmd->file, rcmd->flags, rcmd->mode)) < 0)
                {
                    perror("open");
                    exit(EXIT_FAILURE);
                }
                if (rcmd->cmd->type == EXEC)
                    exec_cmd((struct execcmd*) rcmd->cmd);
                else
                    run_cmd(rcmd->cmd);
                exit(EXIT_SUCCESS);
            }
            TRY( waitpid(pid, &status, 0) );
            break;

        case LIST:
            lcmd = (struct listcmd*) cmd;
            run_cmd(lcmd->left);
            run_cmd(lcmd->right);
            break;

        case PIPE:
            pcmd = (struct pipecmd*)cmd;

            if (pipe(p) < 0)
            {
                perror("pipe");
                exit(EXIT_FAILURE);
            }

            // Ejecución del hijo de la izquierda
            block_sigchld();
            if ((pidI = fork_or_panic("fork PIPE left")) == 0)
            {
                TRY( close(STDOUT_FILENO) );
                TRY( dup(p[1]) );
                TRY( close(p[0]) );
                TRY( close(p[1]) );
                if (pcmd->left->type == EXEC) {
                    ecmd = ((struct execcmd*) pcmd->left);
                    if (is_internal_cmd(ecmd->argv[0]) == 1)
						run_internal_cmd(ecmd);
                    else
                        exec_cmd((struct execcmd*) pcmd->left);
                } else
                    run_cmd(pcmd->left);
                exit(EXIT_SUCCESS);
            }

            // Ejecución del hijo de la derecha
            if ((pidD = fork_or_panic("fork PIPE right")) == 0)
            {
                TRY( close(STDIN_FILENO) );
                TRY( dup(p[0]) );
                TRY( close(p[0]) );
                TRY( close(p[1]) );
				//Comprobar si es interno
                if (pcmd->right->type == EXEC) {
                    ecmd = ((struct execcmd*) pcmd->right);
                    if (is_internal_cmd(ecmd->argv[0]) == 1)
						run_internal_cmd(ecmd);
                    else
                        exec_cmd((struct execcmd*) pcmd->right);
                } else
                    run_cmd(pcmd->right);
                exit(EXIT_SUCCESS);
            }
            TRY( close(p[0]) );
            TRY( close(p[1]) );

            // Esperar a ambos hijos
            TRY( waitpid(pidI, &status, 0) );
            TRY( waitpid(pidD, &status, 0) );
            unblock_sigchld();
            break;

        case BACK:

            sigfillset(&mask_all);
            sigemptyset(&mask_one);
            sigaddset(&mask_one, SIGCHLD);

            bcmd = (struct backcmd*)cmd;

            sigprocmask(SIG_BLOCK, &mask_one, &prev_one);
            if ((pid = fork_or_panic("fork BACK")) == 0)
            {   
                sigprocmask(SIG_SETMASK, &prev_one, NULL);
                if (bcmd->cmd->type == EXEC) {
                    ecmd = ((struct execcmd*) bcmd->cmd);
                    if (is_internal_cmd(ecmd->argv[0]) == 1)
                        run_internal_cmd(ecmd);
                    else
                        exec_cmd((struct execcmd*) bcmd->cmd);
                } else
                    run_cmd(bcmd->cmd);

                exit(EXIT_SUCCESS);
            }
            printf("[%d]\n", pid);

            sigprocmask(SIG_BLOCK, &mask_all, NULL);
            for(int i = 0; i<NUM_BG_PIDS; i++) 
            {
                if(!BG_PIDS[i]) 
                {
                    BG_PIDS[i] = pid;
                    break;
                }
            }
            sigprocmask(SIG_SETMASK, &prev_one, NULL);

            break;

        case SUBS:
            scmd = (struct subscmd*) cmd;
            if ((pid =fork_or_panic("fork SUBS")) == 0) {
                run_cmd(scmd->cmd);
                exit(EXIT_SUCCESS);
            }
            TRY( waitpid(pid, &status, 0) );
            break;

        case INV:
        default:
            panic("%s: estructura `cmd` desconocida\n", __func__);
    }

    DPRINTF(DBG_TRACE, "END\n");
}


void print_cmd(struct cmd* cmd)
{
    struct execcmd* ecmd;
    struct redrcmd* rcmd;
    struct listcmd* lcmd;
    struct pipecmd* pcmd;
    struct backcmd* bcmd;
    struct subscmd* scmd;

    if(cmd == 0) return;

    switch(cmd->type)
    {
        case EXEC:
            ecmd = (struct execcmd*) cmd;
            if (ecmd->argv[0] != 0)
                printf("fork( exec( %s ) )", ecmd->argv[0]);
            break;

        case REDR:
            rcmd = (struct redrcmd*) cmd;
            printf("fork( ");
            if (rcmd->cmd->type == EXEC)
                printf("exec ( %s )", ((struct execcmd*) rcmd->cmd)->argv[0]);
            else
                print_cmd(rcmd->cmd);
            printf(" )");
            break;

        case LIST:
            lcmd = (struct listcmd*) cmd;
            print_cmd(lcmd->left);
            printf(" ; ");
            print_cmd(lcmd->right);
            break;

        case PIPE:
            pcmd = (struct pipecmd*) cmd;
            printf("fork( ");
            if (pcmd->left->type == EXEC)
                printf("exec ( %s )", ((struct execcmd*) pcmd->left)->argv[0]);
            else
                print_cmd(pcmd->left);
            printf(" ) => fork( ");
            if (pcmd->right->type == EXEC)
                printf("exec ( %s )", ((struct execcmd*) pcmd->right)->argv[0]);
            else
                print_cmd(pcmd->right);
            printf(" )");
            break;

        case BACK:
            bcmd = (struct backcmd*) cmd;
            printf("fork( ");
            if (bcmd->cmd->type == EXEC)
                printf("exec ( %s )", ((struct execcmd*) bcmd->cmd)->argv[0]);
            else
                print_cmd(bcmd->cmd);
            printf(" )");
            break;

        case SUBS:
            scmd = (struct subscmd*) cmd;
            printf("fork( ");
            print_cmd(scmd->cmd);
            printf(" )");
            break;

        case INV:
        default:
            panic("%s: estructura `cmd` desconocida\n", __func__);
    }
}


/******************************************************************************
 * Lectura de la línea de órdenes con la biblioteca libreadline
 ******************************************************************************/


// `get_cmd` muestra un *prompt* y lee lo que el usuario escribe usando la
// biblioteca readline. Ésta permite mantener el historial, utilizar las flechas
// para acceder a las órdenes previas del historial, búsquedas de órdenes, etc.

char* get_cmd()
{
    char* buf;

    uid_t uid = getuid();
    struct passwd* passwd = getpwuid(uid);

    if(!passwd) 
    {
	    perror("getpwuid");
	    exit(EXIT_FAILURE);
    }
    char* user = passwd -> pw_name;

    char path[PATH_MAX];
    if(!getcwd(path, PATH_MAX))
    {
	    perror("getcwd");
	    exit(EXIT_FAILURE);
    }

    char *dir = basename(path);
    char prompt[strlen(user)+strlen(dir)+4];
    sprintf(prompt, "%s@%s> ", user, dir);

    // Lee la orden tecleada por el usuario
    buf = readline(prompt);

    // Si el usuario ha escrito una orden, almacenarla en la historia.
    if(buf) add_history(buf);

    return buf;
}

/******************************************************************************
 * Bucle principal de `simplesh`
 ******************************************************************************/


void help(char **argv)
{
    info("Usage: %s [-d N] [-h]\n\
         shell simplesh v%s\n\
         Options: \n\
         -d set debug level to N\n\
         -h help\n\n",
         argv[0], VERSION);
}


void parse_args(int argc, char** argv)
{
    int option;

    // Bucle de procesamiento de parámetros
    while((option = getopt(argc, argv, "d:h")) != -1) {
        switch(option) {
            case 'd':
                g_dbg_level = atoi(optarg);
                break;
            case 'h':
            default:
                help(argv);
                exit(EXIT_SUCCESS);
                break;
        }
    }
}

void block_sigint()
{
    sigset_t blocked_signals;
    sigemptyset(&blocked_signals);
    sigaddset(&blocked_signals, SIGINT);

    if (sigprocmask(SIG_BLOCK, &blocked_signals, NULL) == -1)
    {
        perror("sigprocmask");
        exit(EXIT_FAILURE);
    }
}

void ignore_sigquit()
{
    struct sigaction sa;
    sa.sa_handler = SIG_IGN;
    if (sigaction(SIGQUIT, &sa, NULL) == -1)
    {
        perror("sigaction");
        exit(EXIT_FAILURE);
    }
}

void register_sigchld_handler()
{
    /* Manejador de señales para SIGCHLD */
    struct sigaction sa;
    memset(&sa.sa_flags, 0, sizeof(int));

    sa.sa_handler = &handle_sigchld;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;

    if (sigaction(SIGCHLD, &sa, 0) == -1) 
    {
        perror("sigaction");
        exit(EXIT_FAILURE);
    }
}

int main(int argc, char** argv)
{
    // Register the SIGCHLD handler 
    register_sigchld_handler();

    // Block SIGINT 
    block_sigint();

    // Ignore SIGQUIT 
    ignore_sigquit();
    
    if (unsetenv("OLDPWD") != 0) 
    {
        perror("unset error");
        exit(EXIT_FAILURE);
    }

    char* buf;

    parse_args(argc, argv);

    DPRINTF(DBG_TRACE, "STR\n");

    // Bucle de lectura y ejecución de órdenes
    while ((buf = get_cmd()) != NULL)
    {
        // Realiza el análisis sintáctico de la línea de órdenes
        cmd = parse_cmd(buf);

        // Termina en `NULL` todas las cadenas de las estructuras `cmd`
        null_terminate(cmd);

        DBLOCK(DBG_CMD, {
            info("%s:%d:%s: print_cmd: ",
                 __FILE__, __LINE__, __func__);
            print_cmd(cmd); printf("\n"); fflush(NULL); } );

        // Ejecuta la línea de órdenes
        run_cmd(cmd);

        // Libera la memoria de las estructuras `cmd`
        free_cmd(cmd);

        // Libera la memoria de la línea de órdenes
        free(buf);
    }

    DPRINTF(DBG_TRACE, "END\n");

    return 0;
}
