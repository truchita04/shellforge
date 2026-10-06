#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include <unistd.h>
#include <fcntl.h>

#include <sys/types.h>
#include <sys/wait.h>

#include <signal.h>

#include "parser.h"
#include "executor.h"
#include "builtin.h"
#include "jobs.h"


/* =========================================================
   SIGCHLD HANDLER

   Reaps background children so that they do not become
   zombie processes.
   ========================================================= */

static void sigchld_handler(int sig)
{
    int saved_errno = errno;

    (void)sig;

    while (waitpid(-1, NULL, WNOHANG) > 0)
    {
        /*
         * Reap completed child.
         */
    }

    errno = saved_errno;
}


/* =========================================================
   SETUP SIGCHLD HANDLER
   ========================================================= */

void setup_background_handler(void)
{
    struct sigaction sa;

    memset(&sa, 0, sizeof(sa));

    sa.sa_handler = sigchld_handler;

    sigemptyset(&sa.sa_mask);

    sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;

    if (sigaction(SIGCHLD, &sa, NULL) < 0)
    {
        perror("sigaction");
    }
}


/* =========================================================
   BLOCK SIGCHLD

   Used while creating/waiting for foreground processes.
   ========================================================= */

static int block_sigchld(sigset_t *old_mask)
{
    sigset_t set;

    sigemptyset(&set);

    sigaddset(&set, SIGCHLD);

    if (sigprocmask(
            SIG_BLOCK,
            &set,
            old_mask
        ) < 0)
    {
        perror("sigprocmask");

        return -1;
    }

    return 0;
}


/* =========================================================
   RESTORE SIGNAL MASK
   ========================================================= */

static void restore_signal_mask(
    const sigset_t *old_mask
)
{
    if (sigprocmask(
            SIG_SETMASK,
            old_mask,
            NULL
        ) < 0)
    {
        perror("sigprocmask");
    }
}


/* =========================================================
   APPLY INPUT/OUTPUT REDIRECTION
   ========================================================= */

static int apply_redirection(command_t *cmd)
{
    int fd;


    /* =====================================================
       INPUT REDIRECTION <
       ===================================================== */

    if (cmd->input[0] != '\0')
    {
        fd = open(
            cmd->input,
            O_RDONLY
        );

        if (fd < 0)
        {
            perror(cmd->input);

            return -1;
        }


        if (dup2(
                fd,
                STDIN_FILENO
            ) < 0)
        {
            perror("dup2 input");

            close(fd);

            return -1;
        }


        close(fd);
    }


    /* =====================================================
       OUTPUT REDIRECTION > or >>
       ===================================================== */

    if (cmd->output[0] != '\0')
    {
        if (cmd->append)
        {
            /* >> */

            fd = open(
                cmd->output,
                O_WRONLY |
                O_CREAT |
                O_APPEND,
                0644
            );
        }
        else
        {
            /* > */

            fd = open(
                cmd->output,
                O_WRONLY |
                O_CREAT |
                O_TRUNC,
                0644
            );
        }


        if (fd < 0)
        {
            perror(cmd->output);

            return -1;
        }


        if (dup2(
                fd,
                STDOUT_FILENO
            ) < 0)
        {
            perror("dup2 output");

            close(fd);

            return -1;
        }


        close(fd);
    }


    return 0;
}


/* =========================================================
   PREPARE ARGUMENTS FOR execvp()
   ========================================================= */

static void prepare_arguments(
    command_t *cmd,
    char *args[]
)
{
    int i;

    for (i = 0;
         i < cmd->argc;
         i++)
    {
        args[i] = cmd->argv[i];
    }

    args[cmd->argc] = NULL;
}


/* =========================================================
   EXECUTE BUILTIN WITH REDIRECTION

   This is used for foreground built-ins.

   Example:

       pwd > result.txt
       echo hello > result.txt
   ========================================================= */

static int execute_builtin_with_redirection(
    command_t *cmd
)
{
    int saved_stdin;
    int saved_stdout;

    int result;


    /* Save stdin */

    saved_stdin = dup(STDIN_FILENO);

    if (saved_stdin < 0)
    {
        perror("dup stdin");

        return -1;
    }


    /* Save stdout */

    saved_stdout = dup(STDOUT_FILENO);

    if (saved_stdout < 0)
    {
        perror("dup stdout");

        close(saved_stdin);

        return -1;
    }


    /* Apply redirection */

    if (apply_redirection(cmd) < 0)
    {
        close(saved_stdin);

        close(saved_stdout);

        return -1;
    }


    /* Execute builtin */

    result = execute_builtin(cmd);


    /* Restore stdin */

    if (dup2(
            saved_stdin,
            STDIN_FILENO
        ) < 0)
    {
        perror("restore stdin");
    }


    /* Restore stdout */

    if (dup2(
            saved_stdout,
            STDOUT_FILENO
        ) < 0)
    {
        perror("restore stdout");
    }


    close(saved_stdin);

    close(saved_stdout);


    return result;
}


/* =========================================================
   EXECUTE SINGLE COMMAND
   ========================================================= */

int execute_command(command_t *cmd)
{
    pid_t pid;

    int status;

    sigset_t old_mask;


    if (cmd == NULL)
    {
        return -1;
    }


    if (cmd->argc == 0)
    {
        return 0;
    }


    /* =====================================================
       FOREGROUND BUILTIN
       ===================================================== */

    if (is_builtin(cmd) &&
        !cmd->background)
    {
        return execute_builtin_with_redirection(cmd);
    }


    /* =====================================================
       BLOCK SIGCHLD FOR FOREGROUND COMMAND
       ===================================================== */

    if (!cmd->background)
    {
        if (block_sigchld(
                &old_mask
            ) < 0)
        {
            return -1;
        }
    }


    /* =====================================================
       FORK
       ===================================================== */

    pid = fork();


    if (pid < 0)
    {
        perror("fork");

        if (!cmd->background)
        {
            restore_signal_mask(
                &old_mask
            );
        }

        return -1;
    }


    /* =====================================================
       CHILD
       ===================================================== */

    if (pid == 0)
    {
        char *args[MAX_ARGS + 1];


        /*
 #include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>

#include "executor.h"
#include "builtin.h"


/* =========================================================
   EXECUTE SINGLE COMMAND
   ========================================================= */

int execute_command(command_t *cmd)
{
    pid_t pid;
    int status;

    if (cmd == NULL || cmd->argc == 0)
    {
        return -1;
    }


    /* Built-in command */

    if (is_builtin(cmd))
    {
        return execute_builtin(cmd);
    }


    /* Create child */

    pid = fork();

    if (pid < 0)
    {
        perror("fork");
        return -1;
    }


    /* Child process */

    if (pid == 0)
    {
        char *args[MAX_ARGS + 1];

        for (int i = 0; i < cmd->argc; i++)
        {
            args[i] = cmd->argv[i];
        }

        args[cmd->argc] = NULL;


        execvp(args[0], args);


        perror(args[0]);

        _exit(127);
    }


    /* Parent process */

    if (waitpid(pid, &status, 0) < 0)
    {
        perror("waitpid");
        return -1;
    }


    if (WIFEXITED(status))
    {
        return WEXITSTATUS(status);
    }


    return -1;
}


/* =========================================================
   EXECUTE PIPELINE
   ========================================================= */

int execute_pipeline(pipeline_t *pipeline)
{
    int previous_read = -1;

    pid_t pids[MAX_COMMANDS];

    int command_count;


    if (pipeline == NULL)
    {
        return -1;
    }


    command_count = pipeline->command_count;


    if (command_count == 0)
    {
        return -1;
    }


    /*
     * If there is only one command,
     * execute it normally.
     */

    if (command_count == 1)
    {
        return execute_command(&pipeline->commands[0]);
    }


    /*
     * Multi-command pipeline
     */

    for (int i = 0; i < command_count; i++)
    {
        int pipefd[2];


        /*
         * Create pipe unless this is
         * the last command.
         */

        if (i < command_count - 1)
        {
            if (pipe(pipefd) == -1)
            {
                perror("pipe");
                return -1;
            }
        }


        /*
         * Create child process
         */

        pids[i] = fork();

        if (pids[i] < 0)
        {
            perror("fork");
            return -1;
        }


        /* ================================================
           CHILD PROCESS
           ================================================ */

        if (pids[i] == 0)
        {
            command_t *cmd =
                &pipeline->commands[i];


            /*
             * --------------------------------------------
             * INPUT REDIRECTION
             * --------------------------------------------
             *
             * If this is not the first command,
             * receive input from the previous pipe.
             */

            if (previous_read != -1)
            {
                if (dup2(previous_read,
                         STDIN_FILENO) == -1)
                {
                    perror("dup2 input");
                    _exit(EXIT_FAILURE);
                }
            }


            /*
             * --------------------------------------------
             * OUTPUT REDIRECTION
             * --------------------------------------------
             *
             * If this is not the last command,
             * send output into the current pipe.
             */

            if (i < command_count - 1)
            {
                if (dup2(pipefd[1],
                         STDOUT_FILENO) == -1)
                {
                    perror("dup2 output");
                    _exit(EXIT_FAILURE);
                }
            }


            /*
             * Close inherited descriptors.
             */

            if (previous_read != -1)
            {
                close(previous_read);
            }


            if (i < command_count - 1)
            {
                close(pipefd[0]);
                close(pipefd[1]);
            }


            /*
             * Convert Shellforge argv
             * into execvp argument format.
             */

            char *args[MAX_ARGS + 1];

            for (int j = 0;
                 j < cmd->argc;
                 j++)
            {
                args[j] = cmd->argv[j];
            }

            args[cmd->argc] = NULL;


            /*
             * Built-ins inside a pipe.
             *
             * Note:
             * cd inside a pipeline will only
             * affect this child process.
             */

            if (is_builtin(cmd))
            {
                int result =
                    execute_builtin(cmd);

                _exit(result == 0 ? 0 : 1);
            }


            /*
             * Execute external command.
             */

            execvp(args[0], args);


            perror(args[0]);

            _exit(127);
        }


        /* ================================================
           PARENT PROCESS
           ================================================ */


        /*
         * Parent no longer needs
         * the previous pipe read end.
         */

        if (previous_read != -1)
        {
            close(previous_read);
        }


        /*
         * Parent keeps the read end of the
         * current pipe for the next command.
         */

        if (i < command_count - 1)
        {
            close(pipefd[1]);

            previous_read = pipefd[0];
        }
        else
        {
            previous_read = -1;
        }
    }


    /*
     * Wait for every child process.
     */

    int final_status = 0;

    for (int i = 0; i < command_count; i++)
    {
        int status;

        if (waitpid(pids[i],
                    &status,
                    0) < 0)
        {
            perror("waitpid");
            continue;
        }


        /*
         * Save status of the last command.
         */

        if (i == command_count - 1)
        {
            if (WIFEXITED(status))
            {
                final_status =
                    WEXITSTATUS(status);
            }
        }
    }


    return final_status;
}

