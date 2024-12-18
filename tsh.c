/*
 * tsh - A tiny shell program with job control
 *
 * Spencer Iannantuono
 */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <ctype.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>

/* Misc manifest constants */
#define MAXLINE 1024   /* max line size */
#define MAXARGS 128    /* max args on a command line */
#define MAXJOBS 16     /* max jobs at any point in time */
#define MAXJID 1 << 16 /* max job ID */

/* Job states */
#define UNDEF 0 /* undefined */
#define FG 1    /* running in foreground */
#define BG 2    /* running in background */
#define ST 3    /* stopped */

/*
 * Jobs states: FG (foreground), BG (background), ST (stopped)
 * Job state transitions and enabling actions:
 *     FG -> ST  : ctrl-z
 *     ST -> FG  : fg command
 *     ST -> BG  : bg command
 *     BG -> FG  : fg command
 * At most 1 job can be in the FG state.
 */

/* Global variables */
extern char **environ;   /* defined in libc */
char prompt[] = "tsh> "; /* command line prompt (DO NOT CHANGE) */
int verbose = 0;         /* if true, print additional output */
int nextjid = 1;         /* next job ID to allocate */
char sbuf[MAXLINE];      /* for composing sprintf messages */

struct job_t
{                          /* The job struct */
    pid_t pid;             /* job PID */
    int jid;               /* job ID [1, 2, ...] */
    int state;             /* UNDEF, BG, FG, or ST */
    char cmdline[MAXLINE]; /* command line */
};
struct job_t jobs[MAXJOBS]; /* The job list */
/* End global variables */

/* Function prototypes */

/* Here are the functions that you will implement */
void eval(char *cmdline);
int builtin_cmd(char **argv);
void do_bgfg(char **argv);
void waitfg(pid_t pid);

void sigchld_handler(int sig);
void sigint_handler(int sig);
void sigtstp_handler(int sig);

/* Here are helper routines that we've provided for you */
int parseline(const char *cmdline, char **argv, char **infile, char **outfile, char **errfile, int *append_out);
void sigquit_handler(int sig);

void clearjob(struct job_t *job);
void initjobs(struct job_t *jobs);
int maxjid(struct job_t *jobs);
int addjob(struct job_t *jobs, pid_t pid, int state, char *cmdline);
int deletejob(struct job_t *jobs, pid_t pid);
pid_t fgpid(struct job_t *jobs);
struct job_t *getjobpid(struct job_t *jobs, pid_t pid);
struct job_t *getjobjid(struct job_t *jobs, int jid);
int pid2jid(pid_t pid);
void listjobs(struct job_t *jobs);

void usage(void);
void unix_error(char *msg);
void app_error(char *msg);
typedef void handler_t(int);
handler_t *Signal(int signum, handler_t *handler);

/*
 * main - The shell's main routine
 */
int main(int argc, char **argv)
{
    char c;
    char cmdline[MAXLINE];
    int emit_prompt = 1; /* emit prompt (default) */

    /* Redirect stderr to stdout (so that driver will get all output
     * on the pipe connected to stdout) */
    dup2(1, 2);

    /* Parse the command line */
    while ((c = getopt(argc, argv, "hvp")) != EOF)
    {
        switch (c)
        {
        case 'h': /* print help message */
            usage();
            break;
        case 'v': /* emit additional diagnostic info */
            verbose = 1;
            break;
        case 'p':            /* don't print a prompt */
            emit_prompt = 0; /* handy for automatic testing */
            break;
        default:
            usage();
        }
    }

    /* Install the signal handlers */

    /* These are the ones you will need to implement */
    Signal(SIGINT, sigint_handler);   /* ctrl-c */
    Signal(SIGTSTP, sigtstp_handler); /* ctrl-z */
    Signal(SIGCHLD, sigchld_handler); /* Terminated or stopped child */

    /* This one provides a clean way to kill the shell */
    Signal(SIGQUIT, sigquit_handler);

    /* Initialize the job list */
    initjobs(jobs);

    /* Execute the shell's read/eval loop */
    while (1)
    {

        /* Read command line */
        if (emit_prompt)
        {
            printf("%s", prompt);
            fflush(stdout);
        }
        if ((fgets(cmdline, MAXLINE, stdin) == NULL) && ferror(stdin))
            app_error("fgets error");
        if (feof(stdin))
        { /* End of file (ctrl-d) */
            fflush(stdout);
            exit(0);
        }

        /* Evaluate the command line */
        eval(cmdline);
        fflush(stdout);
        fflush(stdout);
    }

    exit(0); /* control never reaches here */
}

// for pipes
int parsepipe(const char *cmdline, char **commands)
{
    char *cmd = strdup(cmdline); // Copy cmdline to a modifiable string
    char *next = strtok(cmd, "|");
    int count = 0;

    while (next != NULL)
    {
        commands[count++] = strdup(next); // Store each command
        next = strtok(NULL, "|");
    }

    commands[count] = NULL; // Null-terminate the array
    free(cmd);
    return count; // Return the number of commands in the pipeline
}

/*
 * eval - Evaluate the command line that the user has just typed in
 *
 * If the user has requested a built-in command (quit, jobs, bg or fg)
 * then execute it immediately. Otherwise, fork a child process and
 * run the job in the context of the child. If the job is running in
 * the foreground, wait for it to terminate and then return.  Note:
 * each child process must have a unique process group ID so that our
 * background children don't receive SIGINT (SIGTSTP) from the kernel
 * when we type ctrl-c (ctrl-z) at the keyboard.
 */
void eval(char *cmdline)
{
    char *commands[MAXARGS];               // Store pipeline commands
    char buf[MAXLINE];                     // Holds modified command line
    char *argv[MAXARGS];                   // Argument list for execve()
    char *infile, *outfile, *errfile;      // File names for redirection
    int append_out = 0;                    // Append mode flag
    int num_commands;                      // Number of pipeline commands
    int bg;                                // Should the job run in bg or fg?
    pid_t pid;                             // Process id
    int pipefds[2 * MAXARGS];              // Pipe file descriptors
    sigset_t mask_all, mask_one, prev_one; // Signal masks for blocking/unblocking signals

    strcpy(buf, cmdline);
    num_commands = parsepipe(buf, commands); // Split the command into pipeline components

    if (num_commands == 1) // Single command, no pipe
    {
        bg = parseline(buf, argv, &infile, &outfile, &errfile, &append_out);
        if (argv[0] == NULL)
            return; // Ignore empty lines

        if (!builtin_cmd(argv)) // Check for built-in commands first
        {
            sigfillset(&mask_all);
            sigemptyset(&mask_one);
            sigaddset(&mask_one, SIGCHLD);
            sigprocmask(SIG_BLOCK, &mask_one, &prev_one);

            if ((pid = fork()) == 0) // Child process
            {
                sigprocmask(SIG_SETMASK, &prev_one, NULL);
                setpgid(0, 0);

                // Handle input redirection
                if (infile)
                {
                    int fd = open(infile, O_RDONLY);
                    if (fd < 0)
                    {
                        perror("open error for input redirection");
                        exit(1);
                    }
                    dup2(fd, STDIN_FILENO);
                    close(fd);
                }

                // Handle output redirection
                if (outfile)
                {
                    int fd = open(outfile, O_WRONLY | O_CREAT | (append_out ? O_APPEND : O_TRUNC), 0644);
                    if (fd < 0)
                    {
                        perror("open error for output redirection");
                        exit(1);
                    }
                    dup2(fd, STDOUT_FILENO);
                    close(fd);
                }

                // Handle error redirection
                if (errfile)
                {
                    int fd = open(errfile, O_WRONLY | O_CREAT | O_TRUNC, 0644);
                    if (fd < 0)
                    {
                        perror("open error for error redirection");
                        exit(1);
                    }
                    dup2(fd, STDERR_FILENO);
                    close(fd);
                }

                // Execute the command
                if (execvp(argv[0], argv) < 0)
                {
                    perror("Command execution error");
                    exit(1);
                }
            }

            addjob(jobs, pid, bg ? BG : FG, cmdline);
            sigprocmask(SIG_SETMASK, &prev_one, NULL);

            if (!bg)
            {
                waitfg(pid); // Wait for foreground job to finish
            }
            else
            {
                printf("[%d] (%d) %s", pid2jid(pid), pid, cmdline); // Print background job details
            }
        }
    }
    else // Handle pipelines
    {
        int i, status;

        // Create pipes
        for (i = 0; i < num_commands - 1; i++)
        {
            if (pipe(pipefds + i * 2) < 0)
            {
                perror("pipe error");
                exit(1);
            }
        }

        for (i = 0; i < num_commands; i++)
        {
            bg = parseline(commands[i], argv, &infile, &outfile, &errfile, &append_out);

            if ((pid = fork()) == 0) // Child process
            {
                // Set up pipes
                if (i > 0) // Not the first command; get input from the previous pipe
                {
                    dup2(pipefds[(i - 1) * 2], STDIN_FILENO);
                }
                if (i < num_commands - 1) // Not the last command; output to the next pipe
                {
                    dup2(pipefds[i * 2 + 1], STDOUT_FILENO);
                }

                // Close all pipe file descriptors
                for (int j = 0; j < 2 * (num_commands - 1); j++)
                {
                    close(pipefds[j]);
                }

                // Handle input redirection for the first command
                if (i == 0 && infile)
                {
                    int fd = open(infile, O_RDONLY);
                    if (fd < 0)
                    {
                        perror("open error for input redirection");
                        exit(1);
                    }
                    dup2(fd, STDIN_FILENO);
                    close(fd);
                }

                // Handle output redirection for the last command
                if (i == num_commands - 1 && outfile)
                {
                    int fd = open(outfile, O_WRONLY | O_CREAT | (append_out ? O_APPEND : O_TRUNC), 0644);
                    if (fd < 0)
                    {
                        perror("open error for output redirection");
                        exit(1);
                    }
                    dup2(fd, STDOUT_FILENO);
                    close(fd);
                }

                // Execute the command
                if (execvp(argv[0], argv) < 0)
                {
                    perror("Command execution error");
                    exit(1);
                }
            }
        }

        // Close all pipe file descriptors in the parent
        for (i = 0; i < 2 * (num_commands - 1); i++)
        {
            close(pipefds[i]);
        }

        // Wait for all child processes to finish
        for (i = 0; i < num_commands; i++)
        {
            wait(&status);
        }
    }
}

/*
 * parseline - Parse the command line and build the argv array.
 *
 * Characters enclosed in single quotes are treated as a single
 * argument.  Return true if the user has requested a BG job, false if
 * the user has requested a FG job.
 */
int parseline(const char *cmdline, char **argv, char **infile, char **outfile, char **errfile, int *append_out)
{
    static char array[MAXLINE]; // Holds local copy of command line
    char *buf = array;          // Pointer that traverses command line
    char *delim;                // Points to first delimiter
    int argc;                   // Number of args
    int bg;                     // Background job?

    *infile = NULL;
    *outfile = NULL;
    *errfile = NULL;
    *append_out = 0; // Initialize to 0 (overwrite mode)

    strcpy(buf, cmdline);
    buf[strlen(buf) - 1] = ' ';   // Replace trailing '\n' with space
    while (*buf && (*buf == ' ')) // Ignore leading spaces
        buf++;

    argc = 0;

    while (*buf)
    {
        // Skip over spaces
        while (*buf == ' ')
            buf++;

        if (*buf == '\0')
            break;

        if (strncmp(buf, "2>", 2) == 0)
        {
            buf += 2;
            while (*buf == ' ')
                buf++;
            delim = strchr(buf, ' ');
            if (delim)
                *delim = '\0';
            *errfile = buf;
            if (delim)
                buf = delim + 1;
            else
                buf += strlen(buf);
        }
        else if (*buf == '<')
        {
            buf++;
            while (*buf == ' ')
                buf++;
            delim = strchr(buf, ' ');
            if (delim)
                *delim = '\0';
            *infile = buf;
            if (delim)
                buf = delim + 1;
            else
                buf += strlen(buf);
        }
        else if (*buf == '>')
        {
            buf++;
            if (*buf == '>')
            {
                buf++;
                *append_out = 1; // Set append mode
            }
            while (*buf == ' ')
                buf++;
            delim = strchr(buf, ' ');
            if (delim)
                *delim = '\0';
            *outfile = buf;
            if (delim)
                buf = delim + 1;
            else
                buf += strlen(buf);
        }
        else
        {
            // Regular argument
            argv[argc++] = buf;
            delim = strchr(buf, ' ');
            if (delim)
            {
                *delim = '\0';
                buf = delim + 1;
            }
            else
            {
                buf += strlen(buf);
            }
        }
    }

    argv[argc] = NULL;

    if (argc == 0) // Ignore blank line
        return 1;

    if ((bg = (*argv[argc - 1] == '&')) != 0)
    {
        argv[--argc] = NULL;
    }

    return bg;
}

/*
 * builtin_cmd - If the user has typed a built-in command then execute
 *    it immediately.
 */
int builtin_cmd(char **argv)
{
    // For quit command
    if (strcmp(argv[0], "quit") == 0)
    {
        exit(0);
    }
    // For jobs command
    else if (strcmp(argv[0], "jobs") == 0)
    {
        listjobs(jobs);
        return 1;
    }
    // For bg and fg commands
    else if (strcmp(argv[0], "bg") == 0 || strcmp(argv[0], "fg") == 0)
    {
        do_bgfg(argv);
        return 1;
    }
    return 0; // Not a built-in command
}

/*
 * do_bgfg - Execute the builtin bg and fg commands
 */
void do_bgfg(char **argv)
{
    struct job_t *job;  // Pointer to the job structure
    char *id = argv[1]; // The job ID or process ID argument
    int jid;            // Job ID
    pid_t pid;          // Process ID

    // Check if the argument is provided
    if (id == NULL)
    {
        printf("%s command requires PID or %%jobid argument\n", argv[0]);
        return;
    }

    // Check if the argument is a job ID (starts with '%')
    if (id[0] == '%')
    {
        jid = atoi(&id[1]);         // Convert job ID string to integer
        job = getjobjid(jobs, jid); // Get the job structure by job ID
        if (job == NULL)
        {
            printf("%s: No such job\n", id);
            return;
        }
        pid = job->pid; // Get the process ID from the job structure
    }
    // Check if the argument is a process ID (a digit)
    else if (isdigit(id[0]))
    {
        pid = atoi(id);             // Convert process ID string to integer
        job = getjobpid(jobs, pid); // Get the job structure by process ID
        if (job == NULL)
        {
            printf("(%d): No such process\n", pid);
            return;
        }
    }
    // Invalid argument format
    else
    {
        printf("%s: argument must be a PID or %%jobid\n", argv[0]);
        return;
    }

    // Send the SIGCONT signal to the job's process group
    if (kill(-pid, SIGCONT) < 0)
        unix_error("kill (SIGCONT) error");

    // If the command is 'fg', bring the job to the foreground
    if (strcmp(argv[0], "fg") == 0)
    {
        job->state = FG;  // Set job state to foreground
        waitfg(job->pid); // Wait for the job to finish
    }
    // If the command is 'bg', resume the job in the background
    else if (strcmp(argv[0], "bg") == 0)
    {
        job->state = BG;                                          // Set job state to background
        printf("[%d] (%d) %s", job->jid, job->pid, job->cmdline); // Print job details
    }
}

/*
 * waitfg - Block until process pid is no longer the foreground process
 */
void waitfg(pid_t pid)
{
    while (pid == fgpid(jobs))
    {
        usleep(1000); // Sleep for 1 millisecond
    }
}

/*****************
 * Signal handlers
 *****************/

/*
 * sigchld_handler - The kernel sends a SIGCHLD to the shell whenever
 *     a child job terminates (becomes a zombie), or stops because it
 *     received a SIGSTOP or SIGTSTP signal. The handler reaps all
 *     available zombie children, but doesn't wait for any other
 *     currently running children to terminate.
 */
void sigchld_handler(int sig)
{
    int olderrno = errno;        // Save the old errno value
    sigset_t mask_all, prev_all; // Signal masks for blocking/unblocking signals
    pid_t pid;
    int status;

    sigfillset(&mask_all); // Initialize mask_all to block all signals
    // // check for error
    // if (sigfillset(&mask_all) < 0)
    // {
    //     unix_error("sigfillset error");
    // }

    // Reap all available zombie children
    while ((pid = waitpid(-1, &status, WNOHANG | WUNTRACED)) > 0)
    {
        sigprocmask(SIG_BLOCK, &mask_all, &prev_all); // Block all signals
        // check for error
        // if (sigprocmask(SIG_BLOCK, &mask_all, &prev_all) < 0)
        // {
        //     unix_error("sigprocmask error");
        // }

        // Check if the child was stopped by a signal
        if (WIFSTOPPED(status))
        {
            struct job_t *job = getjobpid(jobs, pid); // Get the job structure by process ID
            if (job != NULL)
            {
                job->state = ST; // Set job state to stopped
                printf("Job [%d] (%d) stopped by signal %d\n", job->jid, job->pid, WSTOPSIG(status));
            }
        }
        // Check if the child was terminated by a signal
        else if (WIFSIGNALED(status))
        {
            struct job_t *job = getjobpid(jobs, pid); // Get the job structure by process ID
            if (job != NULL)
            {
                printf("Job [%d] (%d) terminated by signal %d\n", job->jid, job->pid, WTERMSIG(status));
                deletejob(jobs, pid); // Delete the job from the job list
            }
        }
        // Check if the child exited normally
        else if (WIFEXITED(status))
        {
            deletejob(jobs, pid); // Delete the job from the job list
        }

        sigprocmask(SIG_SETMASK, &prev_all, NULL); // Restore previous signal mask
        // check for error
        // if (sigprocmask(SIG_SETMASK, &prev_all, NULL) < 0)
        // {
        //     unix_error("sigprocmask error");
        // }
    }
    // check for error
    // if (pid < 0 && errno != ECHILD)
    // {
    //     unix_error("waitpid error");
    // }

    errno = olderrno; // Restore the old errno value
}

/*
 * sigint_handler - The kernel sends a SIGINT to the shell whenver the
 *    user types ctrl-c at the keyboard.  Catch it and send it along
 *    to the foreground job.
 *   The shell should send a SIGINT signal to the process group of the foreground job.
 */
void sigint_handler(int sig)
{
    pid_t fg_pid = fgpid(jobs); // Get the PID of the foreground job

    if (fg_pid != 0)
    {
        if (kill(-fg_pid, SIGINT) < 0)
        { // Send SIGINT to the process group
            perror("kill (sigint_handler)");
        }
    }
}

/*
 * sigtstp_handler - The kernel sends a SIGTSTP to the shell whenever
 *     the user types ctrl-z at the keyboard. Catch it and suspend the
 *     foreground job by sending it a SIGTSTP.
 */
void sigtstp_handler(int sig)
{
    pid_t fg_pid = fgpid(jobs); // Get the PID of the foreground job

    if (fg_pid != 0)
    {
        if (kill(-fg_pid, SIGTSTP) < 0)
        { // Send SIGTSTP to the process group
            perror("kill (sigtstp_handler)");
        }
    }
}

/*********************
 * End signal handlers
 *********************/

/***********************************************
 * Helper routines that manipulate the job list
 **********************************************/

/* clearjob - Clear the entries in a job struct */
void clearjob(struct job_t *job)
{
    job->pid = 0;
    job->jid = 0;
    job->state = UNDEF;
    job->cmdline[0] = '\0';
}

/* initjobs - Initialize the job list */
void initjobs(struct job_t *jobs)
{
    int i;

    for (i = 0; i < MAXJOBS; i++)
        clearjob(&jobs[i]);
}

/* maxjid - Returns largest allocated job ID */
int maxjid(struct job_t *jobs)
{
    int i, max = 0;

    for (i = 0; i < MAXJOBS; i++)
        if (jobs[i].jid > max)
            max = jobs[i].jid;
    return max;
}

/* addjob - Add a job to the job list */
int addjob(struct job_t *jobs, pid_t pid, int state, char *cmdline)
{
    int i;

    if (pid < 1)
        return 0;

    for (i = 0; i < MAXJOBS; i++)
    {
        if (jobs[i].pid == 0)
        {
            jobs[i].pid = pid;
            jobs[i].state = state;
            jobs[i].jid = nextjid++;
            if (nextjid > MAXJOBS)
                nextjid = 1;
            strcpy(jobs[i].cmdline, cmdline);
            if (verbose)
            {
                printf("Added job [%d] %d %s\n", jobs[i].jid, jobs[i].pid, jobs[i].cmdline);
            }
            return 1;
        }
    }
    printf("Tried to create too many jobs\n");
    return 0;
}

/* deletejob - Delete a job whose PID=pid from the job list */
int deletejob(struct job_t *jobs, pid_t pid)
{
    int i;

    if (pid < 1)
        return 0;

    for (i = 0; i < MAXJOBS; i++)
    {
        if (jobs[i].pid == pid)
        {
            clearjob(&jobs[i]);
            nextjid = maxjid(jobs) + 1;
            return 1;
        }
    }
    return 0;
}

/* fgpid - Return PID of current foreground job, 0 if no such job */
pid_t fgpid(struct job_t *jobs)
{
    int i;

    for (i = 0; i < MAXJOBS; i++)
        if (jobs[i].state == FG)
            return jobs[i].pid;
    return 0;
}

/* getjobpid  - Find a job (by PID) on the job list */
struct job_t *getjobpid(struct job_t *jobs, pid_t pid)
{
    int i;

    if (pid < 1)
        return NULL;
    for (i = 0; i < MAXJOBS; i++)
        if (jobs[i].pid == pid)
            return &jobs[i];
    return NULL;
}

/* getjobjid  - Find a job (by JID) on the job list */
struct job_t *getjobjid(struct job_t *jobs, int jid)
{
    int i;

    if (jid < 1)
        return NULL;
    for (i = 0; i < MAXJOBS; i++)
        if (jobs[i].jid == jid)
            return &jobs[i];
    return NULL;
}

/* pid2jid - Map process ID to job ID */
int pid2jid(pid_t pid)
{
    int i;

    if (pid < 1)
        return 0;
    for (i = 0; i < MAXJOBS; i++)
        if (jobs[i].pid == pid)
        {
            return jobs[i].jid;
        }
    return 0;
}

/* listjobs - Print the job list */
void listjobs(struct job_t *jobs)
{
    int i;

    for (i = 0; i < MAXJOBS; i++)
    {
        if (jobs[i].pid != 0)
        {
            printf("[%d] (%d) ", jobs[i].jid, jobs[i].pid);
            switch (jobs[i].state)
            {
            case BG:
                printf("Running ");
                break;
            case FG:
                printf("Foreground ");
                break;
            case ST:
                printf("Stopped ");
                break;
            default:
                printf("listjobs: Internal error: job[%d].state=%d ",
                       i, jobs[i].state);
            }
            printf("%s", jobs[i].cmdline);
        }
    }
}
/******************************
 * end job list helper routines
 ******************************/

/***********************
 * Other helper routines
 ***********************/

/*
 * usage - print a help message
 */
void usage(void)
{
    printf("Usage: shell [-hvp]\n");
    printf("   -h   print this message\n");
    printf("   -v   print additional diagnostic information\n");
    printf("   -p   do not emit a command prompt\n");
    exit(1);
}

/*
 * unix_error - unix-style error routine
 */
void unix_error(char *msg)
{
    fprintf(stdout, "%s: %s\n", msg, strerror(errno));
    exit(1);
}

/*
 * app_error - application-style error routine
 */
void app_error(char *msg)
{
    fprintf(stdout, "%s\n", msg);
    exit(1);
}

/*
 * Signal - wrapper for the sigaction function
 */
handler_t *Signal(int signum, handler_t *handler)
{
    struct sigaction action, old_action;

    action.sa_handler = handler;
    sigemptyset(&action.sa_mask); /* block sigs of type being handled */
    action.sa_flags = SA_RESTART; /* restart syscalls if possible */

    if (sigaction(signum, &action, &old_action) < 0)
        unix_error("Signal error");
    return (old_action.sa_handler);
}

/*
 * sigquit_handler - The driver program can gracefully terminate the
 *    child shell by sending it a SIGQUIT signal.
 */
void sigquit_handler(int sig)
{
    printf("Terminating after receipt of SIGQUIT signal\n");
    exit(1);
}