#include <dirent.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define CMDLINE_MAX 512
#define TOKEN_MAX 32
#define ARG_MAX 16
#define PROCESS_MAX 8

struct process {
        char* argv[ARG_MAX + 1];
        int fd_in;
        int fd_out;
        int fd_err;
};

/* SECTION 0: Error Handling Helper */

/**
 * @brief print error to stderr and return -1
 *
 * @param msg error message
 * @return -1
 */
int error(const char* msg)
{
        fprintf(stderr, "Error: %s\n", msg);
        return -1;
}

/**
 * @brief print a system error message and exit with failure status
 *
 * @param func_name the name of the function that produced the error
 */
void exit_with_sys_err(const char* func_name)
{
        perror(func_name);
        exit(EXIT_FAILURE);
}

/* SECTION 1: Build-in Commands */

/**
 * @brief print the current working directory
 *
 * @return 0 if successful; -1 otherwise
 */
int pwd(void)
{
        char cwd[PATH_MAX];

        if (!getcwd(cwd, PATH_MAX))
                return -1;

        printf("%s\n", cwd);
        return 0;
}

/**
 * @brief change the working directory
 *
 * @param path path of the working directory to change to
 * @return 0 if successful; -1 otherwise
 */
int cd(const char* path)
{
        if (chdir(path))
                return error("cannot cd into directory");

        return 0;
}

/**
 * @brief list contents in current directory
 *
 * @return 0 if successful; -1 otherwise
 */
int sls(void)
{
        DIR* dir_p = opendir("./");
        struct dirent* ent_p;
        struct stat ent_st;

        if (!dir_p)
                return error("cannot open directory");

        /* iterate over entries in the directory */
        while ((ent_p = readdir(dir_p))) {
                /* get stat for the current entry */
                if (stat(ent_p->d_name, &ent_st)) {
                        perror("sls");
                        return -1;
                }

                /* print filename and size if the file is not hidden */
                if (ent_p->d_name[0] != '.')
                        printf("%s (%ld bytes)\n", ent_p->d_name, ent_st.st_size);
        }

        (void)closedir(dir_p);

        return 0;
}

/* SECTION 2: Commandline Tokenization (Lexical Analysis) */

/**
 * @brief read the next commandline token from a stream of characters.
 *
 * @param iter the current position of the char iterator
 * @param buf the buffer to store the token read
 * @return
 * the pointer pointing to the next charater after the end of the token read;
 * `NULL` when the string terminator is reached.
 */
const char* read_next_token(const char* iter, char* buf) {
        /* move the iterator to the first non-whitespace character */
        while (*iter == ' ' || *iter == '\t')
                iter++;

        /* return NULL when string terminator is reached */
        if (*iter == '\0')
                return NULL;

        /* read the 1st token char */
        *buf = *iter++;

        /* determine the token type by looking at the first character */
        switch (*buf++) {
        /* the token is an operator */
        case '|':
        case '>':
                if (*iter == '&')
                        *buf++ = *iter++;
                break;

        /* the token is an argument */
        default:
                while (*iter && *iter != ' ' && *iter != '\t' && *iter != '|' && *iter != '>')
                        *buf++ = *iter++;
                break;
        }

        /* terminate string */
        *buf = '\0';

        /* return the current position of the iterator */
        return iter;
}

/**
 * @brief read a commandline into tokens
 *
 * @param cmdline the string tokens should be read from
 * @param tokens null-terminated list output of tokens read from the string
 */
void read_tokens(const char* cmdline, char* tokens[])
{
        char token_buf[TOKEN_MAX];

        while ((cmdline = read_next_token(cmdline, token_buf))) {
                *tokens = malloc(TOKEN_MAX * sizeof(char));

                /* check malloc failure */
                if (!*tokens)
                        exit(EXIT_FAILURE);

                strcpy(*tokens++, token_buf);
        }

        /* terminate token array */
        *tokens = NULL;
}

/**
 * @brief determines if the token is an argument
 */
bool is_arg(const char* token)
{
        return token && token[0] != '>' && token[0] != '|';
}

/**
 * @brief determines if the token is a pipe token
 */
bool is_pipe_token(const char* token)
{
        return token && token[0] == '|';
}

/**
 * @brief determines if the token is an output redirect token
 */
bool is_out_redirect_token(const char* token)
{
        return token && token[0] == '>';
}

/* SECTION 3: Commandline Parsing */

/**
 * @brief read command arguments from a stream of tokens
 *
 * @param token_iter current position of token iterator
 * @param argv null-terminated list output of arguments read from token stream
 * @return
 * the new position of the token iterator pointing to the next token after the last argument read;
 * `NULL` if there is no or too many arguments.
 */
char** read_argv(char** token_iter, char* argv[])
{
        size_t i = 0;

        while (is_arg(*token_iter)) {
                if (i == ARG_MAX) {
                        (void)error("too many process arguments");
                        return NULL;
                }

                argv[i++] = *token_iter++;
        }

        argv[i] = NULL;

        if (!argv[0]) {
                (void)error("missing command");
                return NULL;
        }

        return token_iter;
}

/**
 * @brief create a pipe between two processes
 *
 * @param src source process that will write to the pipe
 * @param dest destination process that will read from the pipe
 * @param redirect_err whether the error of the source process will be redirected to the pipe
 * @return 0 if pipe creation is successful; -1 otherwise
 */
int pipe_procs(struct process* src, struct process* dest, bool redirect_err)
{
        int pipe_fds[2];

        if (pipe(pipe_fds))
                exit_with_sys_err("pipe");

        src->fd_out = pipe_fds[1];
        src->fd_err = redirect_err ? pipe_fds[1] : STDERR_FILENO;
        dest->fd_in = pipe_fds[0];

        return 0;
}

/**
 * @brief redirect the output of a process to a file
 *
 * @param proc the process for which the output needs to be redirected
 * @param out_file_path the path of the output file
 * @param redirect_err whether the error of the process should be redirected to the output file
 * @return 0 if successful; -1 otherwise
 */
int redirect_proc_out(struct process* proc, const char* out_file_path, bool redirect_err)
{
        /* if the file already exists, truncate the length to 0; */
        /* if the file does not exists, create it with permission rw-r--r--. */
        int fd = open(out_file_path, O_CREAT | O_WRONLY | O_TRUNC, 0644);

        if (fd == -1)
                return error("cannot open output file");

        proc->fd_out = fd;
        proc->fd_err = redirect_err ? fd : STDERR_FILENO;

        return 0;
}

/**
 * @brief parse a commandline into a list of processes
 * each with an argument list and file descriptors for input, output, and error
 *
 * @param cmdline commandline
 * @param procs null-terminated output list of processes
 * @return 0 if successful; -1 otherwise
 */
int parse_command(const char* cmdline, struct process* procs[])
{
        /* tokenize commandline (characters -> tokens) */
        char* tokens[CMDLINE_MAX];
        read_tokens(cmdline, tokens);

        /* initialize token iterator */
        char** tok_iter = tokens;

        /* check if there is no command */
        if (!*tok_iter)
                return -1;

        /* initialize process index */
        size_t i = 0;

        /* allocate memory for the 1st process */
        procs[i] = malloc(sizeof(struct process));

        /* check malloc failure */
        if (!procs[i])
                exit(EXIT_FAILURE);

        /* the input for the 1st process will always be standard input */
        procs[i]->fd_in = STDIN_FILENO;

        while (true) {
                if (!(tok_iter = read_argv(tok_iter, procs[i]->argv)))
                        return -1;

                if (!is_pipe_token(*tok_iter))
                        break;

                procs[++i] = malloc(sizeof(struct process));

                /* check malloc failure */
                if (!procs[i])
                        exit(EXIT_FAILURE);

                bool redirect_err = (*tok_iter++)[1];
                if (pipe_procs(procs[i - 1], procs[i], redirect_err))
                        return -1;
        }

        if (is_out_redirect_token(*tok_iter)) {
                bool redirect_err = (*tok_iter++)[1];
                char* out_file_path = *tok_iter++;

                if (!out_file_path)
                        return error("no output file");

                if (redirect_proc_out(procs[i], out_file_path, redirect_err))
                        return -1;

                if (is_pipe_token(*tok_iter))
                        return error("mislocated output redirection");
        } else {
                procs[i]->fd_out = STDOUT_FILENO;
                procs[i]->fd_err = STDERR_FILENO;
        }

        /* terminate process array */
        procs[i + 1] = NULL;

        free(tokens);

        return 0;
}

/* SECTION 4: Process Execution */

/**
 * @brief close non-standard input, output, and error file descriptors of a process
 */
void close_nonstd_fds(const struct process* proc)
{
        if (proc->fd_in != STDIN_FILENO)
                (void)close(proc->fd_in);

        if (proc->fd_out != STDOUT_FILENO)
                (void)close(proc->fd_out);

        if (proc->fd_err != STDERR_FILENO)
                (void)close(proc->fd_err);
}

/**
 * @brief execute a process (never returns)
 *
 * @param proc the process to be executed
 */
void exec_proc(const struct process* proc)
{
        /* duplicate file descriptors according to what's specified in proc */
        /* dup2 will do nothing if the 2 values supplied are the same */

        if (dup2(proc->fd_in, STDIN_FILENO) == -1)
                exit_with_sys_err("dup2");

        if (dup2(proc->fd_out, STDOUT_FILENO) == -1)
                exit_with_sys_err("dup2");

        if (dup2(proc->fd_err, STDERR_FILENO) == -1)
                exit_with_sys_err("dup2");

        /* close duplicated file descriptors */
        close_nonstd_fds(proc);

        (void)execvp(proc->argv[0], proc->argv);

        (void)error("command not found");
        exit(EXIT_FAILURE);
}

/**
 * @brief run a list of processes and returns when all of them are exited
 *
 * @param procs a list of processes to run
 * @param statuses the -1 ternimated output list of the exit statuses of processes
 */
void run_procs(struct process* procs[], int statuses[])
{
        int pids[PROCESS_MAX];
        size_t i = 0;

        while (procs[i]) {
                pid_t pid = fork();

                if (pid == -1)
                        exit_with_sys_err("fork");

                if (pid) {  /* parent */
                        /**
                         * close file descriptors used by the forked child.
                         * the children to be forked in the next iteration do not need to
                         * close the file descriptors of the processes forked before it.
                         */
                        close_nonstd_fds(procs[i]);

                        /* save the child's pid */
                        pids[i++] = pid;
                } else {  /* child */
                        /**
                         * close file descriptors used by other processes after index i.
                         * since file descriptors for processes before index i are already closed
                         * in previous iterations of this loop by the parent before this fork,
                         * now file descriptors used by ALL other processes are closed.
                         */
                        for (size_t j = i + 1; procs[j]; j++)
                                close_nonstd_fds(procs[j]);

                        exec_proc(procs[i]);
                        return;
                }
        }

        /* terminate array using -1 */
        statuses[i] = -1;

        /* wait for all children to exit */
        while (i--) {
                int status;
                (void)waitpid(pids[i], &status, 0);
                statuses[i] = WEXITSTATUS(status);
        }
}

/* SECTION 5: Main Function */

int main(void)
{
        char cmdline[CMDLINE_MAX];

        /* NULL terminated process list */
        struct process* procs[PROCESS_MAX];

        /* -1 terminated exit statuses for processes */
        int statuses[PROCESS_MAX];

        while (true) {
                char *nl;
                bool exiting = false;

                /* print prompt */
                printf("sshell@ucd$ ");
                fflush(stdout);

                /* get command line */
                fgets(cmdline, CMDLINE_MAX, stdin);

                /* print command line if stdin is not provided by terminal */
                if (!isatty(STDIN_FILENO)) {
                        printf("%s", cmdline);
                        fflush(stdout);
                }

                /* remove trailing newline from command line */
                nl = strchr(cmdline, '\n');
                if (nl)
                        *nl = '\0';

                /* parse command line */
                if (parse_command(cmdline, procs))
                        continue;

                /* determine whether its a built-in command */
                char* first_arg = procs[0]->argv[0];

                if (!strcmp(first_arg, "exit")) {
                        fprintf(stderr, "Bye...\n");
                        statuses[0] = 0;
                        statuses[1] = -1;
                        exiting = true;
                } else if (!strcmp(first_arg, "pwd")) {
                        statuses[0] = pwd() ? 1 : 0;
                        statuses[1] = -1;
                } else if (!strcmp(first_arg, "cd")) {
                        statuses[0] = cd(procs[0]->argv[1]) ? 1 : 0;
                        statuses[1] = -1;
                } else if (!strcmp(first_arg, "sls")) {
                        statuses[0] = sls() ? 1 : 0;
                        statuses[1] = -1;
                } else {
                        run_procs(procs, statuses);
                }

                /* print return statuses and free malloc-ed memory */
                fprintf(stderr, "+ completed '%s' ", cmdline);
                for (size_t i = 0; statuses[i] != -1; i++) {
                        fprintf(stderr, "[%d]", statuses[i]);
                        free(procs[i]);
                }
                fprintf(stderr, "\n");

                if (exiting)
                        break;
        }

        return EXIT_SUCCESS;
}
