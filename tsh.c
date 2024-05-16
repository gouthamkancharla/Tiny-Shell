/* 
 * tsh - A tiny shell program with job control
 * 
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

/* Misc manifest constants */
#define MAXLINE    1024   /* max line size */
#define MAXARGS     128   /* max args on a command line */
#define MAXJOBS      16   /* max jobs at any point in time */

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
extern char **environ;      /* defined in libc */
char prompt[] = "tsh> ";    /* command line prompt (DO NOT CHANGE) */
int verbose = 0;            /* if true, print additional output */
char sbuf[MAXLINE];         /* for composing sprintf messages */

struct job_t {              /* Per-job data */
    pid_t pid;              /* job PID */
    int jid;                /* job ID [1, 2, ...] */
    int state;              /* UNDEF, FG, BG, or ST */
    char cmdline[MAXLINE];  /* command line */
};
struct job_t jobs[MAXJOBS]; /* The job list */

volatile sig_atomic_t ready; /* Is the newest child in its own process group? */

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
int parseline(const char *cmdline, char **argv); 
void sigquit_handler(int sig);
void sigusr1_handler(int sig);

void clearjob(struct job_t *job);
void initjobs(struct job_t *jobs);
int freejid(struct job_t *jobs); 
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
int main(int argc, char **argv) {
    char c;
    char cmdline[MAXLINE];
    int emit_prompt = 1; /* emit prompt (default) */

    /* Redirect stderr to stdout (so that driver will get all output
     * on the pipe connected to stdout) */
    dup2(STDOUT_FILENO, STDERR_FILENO);

    /* Parse the command line */
    while ((c = getopt(argc, argv, "hvp")) != -1) {
        switch (c) {
            case 'h':             /* print help message */
                usage();
                break;
            case 'v':             /* emit additional diagnostic info */
                verbose = 1;
                break;
            case 'p':             /* don't print a prompt */
                emit_prompt = 0;  /* handy for automatic testing */
                break;
            default:
                usage();
        }
    }

    /* Install the signal handlers */

    Signal(SIGUSR1, sigusr1_handler); /* Child is ready */

    /* These are the ones you will need to implement */
    Signal(SIGINT,  sigint_handler);   /* ctrl-c */
    Signal(SIGTSTP, sigtstp_handler);  /* ctrl-z */
    Signal(SIGCHLD, sigchld_handler);  /* Terminated or stopped child */

    /* This one provides a clean way to kill the shell */
    Signal(SIGQUIT, sigquit_handler); 

    /* Initialize the job list */
    initjobs(jobs);

    /* Execute the shell's read/eval loop */
    while (1) {
        /* Read command line */
        if (emit_prompt) {
            printf("%s", prompt);
            fflush(stdout);
        }
        if ((fgets(cmdline, MAXLINE, stdin) == NULL) && ferror(stdin))
            app_error("fgets error");
        if (feof(stdin)) { /* End of file (ctrl-d) */
            fflush(stdout);
            exit(0);
        }

        /* Evaluate the command line */
        eval(cmdline);
        fflush(stdout);
    } 

    exit(0); /* control never reaches here */
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
void eval(char *cmdline) {
    char *argv[MAXARGS];  // Argument list for execve()
    int argc;             // Number of arguments in command line
    pid_t pid;            // Process id

    argc = parseline(cmdline, argv); // Parse command line
    if (argc == 0) {
        return;   // Ignore empty lines
    }

    if (!builtin_cmd(argv)) {
        // Not a built-in command, execute in a child process
        if ((pid = fork()) == 0) {
            // Child process
            setpgid(0, 0);  // Put child in a new process group
            if (*argv[argc-1] == '&') {
                argv[argc-1] = NULL;
            } else {
                // File redirection
                int in_fd = -1; // File descriptor for input redirection
                int out_fd = -1; // File descriptor for output redirection

                for (int i = 0; i < argc; ++i) {
                    if (strcmp(argv[i], "<") == 0) {
                        if (in_fd != -1) {
                            fprintf(stderr, "Syntax error: Multiple input redirection\n");
                            exit(1);
                        }
                        argv[i] = NULL; // Remove the '<' from the argument list
                        in_fd = open(argv[i + 1], O_RDONLY);
                        if (in_fd < 0) {
                            perror("Failed to open input file");
                            exit(1);
                        }
                        if (dup2(in_fd, STDIN_FILENO) < 0) {
                            perror("Failed to redirect input");
                            exit(1);
                        }
                        close(in_fd);
                    } else if (strcmp(argv[i], ">") == 0) {
                        if (out_fd != -1) {
                            fprintf(stderr, "Syntax error: Multiple output redirection\n");
                            exit(1);
                        }
                        argv[i] = NULL; // Remove the '>' from the argument list
                        out_fd = open(argv[i + 1], O_WRONLY | O_CREAT | O_TRUNC, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
                        if (out_fd < 0) {
                            perror("Failed to open output file");
                            exit(1);
                        }
                        if (dup2(out_fd, STDOUT_FILENO) < 0) {
                            perror("Failed to redirect output");
                            exit(1);
                        }
                        close(out_fd);
                    }
                }
            }
            
            if (execve(argv[0], argv, environ) < 0) {
                fprintf(stderr, "%s: Command not found\n", argv[0]);
                exit(0);
            }
        }
        
        // Parent process
        if (*argv[argc-1] == '&') {
            // Background job
            addjob(jobs, pid, BG, cmdline);
            printf("[%d] (%d) %s", pid2jid(pid), pid, cmdline);
        } else {
            // Foreground job
            addjob(jobs, pid, FG, cmdline);
            waitfg(pid);  // Wait for foreground job to terminate
        }
    }
    return;
}

/* 
 * parseline - Parse the command line and build the argv array.
 * 
 * Characters enclosed in single quotes are treated as a single
 * argument.  Return number of arguments parsed.
 */
int parseline(const char *cmdline, char **argv) {
    static char array[MAXLINE]; /* holds local copy of command line */
    char *buf = array;          /* ptr that traverses command line */
    char *delim;                /* points to space or quote delimiters */
    int argc;                   /* number of args */

    strcpy(buf, cmdline);
    buf[strlen(buf)-1] = ' ';  /* replace trailing '\n' with space */
    while (*buf && (*buf == ' ')) /* ignore leading spaces */
        buf++;

    /* Build the argv list */
    argc = 0;
    if (*buf == '\'') {
        buf++;
        delim = strchr(buf, '\'');
    }
    else {
        delim = strchr(buf, ' ');
    }

    while (delim) {
        argv[argc++] = buf;
        *delim = '\0';
        buf = delim + 1;
        while (*buf && (*buf == ' ')) /* ignore spaces */
            buf++;

        if (*buf == '\'') {
            buf++;
            delim = strchr(buf, '\'');
        }
        else {
            delim = strchr(buf, ' ');
        }
    }
    argv[argc] = NULL;
    
    return argc;
}

/* 
 * builtin_cmd - If the user has typed a built-in command then execute
 *    it immediately.  
 */
int builtin_cmd(char **argv) {
    if (strcmp(argv[0], "quit") == 0) {
        exit(0); // Terminate the shell
    } else if (strcmp(argv[0], "jobs") == 0) {
        listjobs(jobs); // List all jobs
        return 1;
    } else if (strcmp(argv[0], "bg") == 0 || strcmp(argv[0], "fg") == 0) {
        do_bgfg(argv); // Execute the bg or fg command
        return 1;
    }
    return 0;     /* not a builtin command */
}

/* 
 * do_bgfg - Execute the builtin bg and fg commands
 */
void do_bgfg(char **argv) {
    struct job_t *job;
    char *id = argv[1];

    if (id == NULL) {
        if (strcmp(argv[0], "bg") == 0) {
            printf("bg command requires PID or %%jid argument\n");
        } else {
            printf("fg command requires PID or %%jid argument\n");
        }
        return;
    }

    // Determine if id is a PID or JID
    int is_jid = (id[0] == '%');

    int i;
    // Get the job based on PID or JID
    if (is_jid) {
        for (i = 1; i < strlen(id); i++) {
            if (!isdigit(id[i])) {
                if (strcmp(argv[0], "bg") == 0) {
                    printf("bg: argument must be a PID or %%jid\n");
                } else {
                    printf("fg: argument must be a PID or %%jid\n");
                }
                return;
            }
        }
        int jid = atoi(&id[1]);
        job = getjobjid(jobs, jid);
        if (!job) {
            printf("%%%d: No such job\n", jid);
            return;
        }
    } else {
        for (i = 0; i < strlen(id); i++) {
            if (!isdigit(id[i])) { 
                if (strcmp(argv[0], "bg") == 0) {
                    printf("bg: argument must be a PID or %%jid\n");
                } else {
                    printf("fg: argument must be a PID or %%jid\n");
                }
                return;
            }
        }
        pid_t pid = atoi(id);
        job = getjobpid(jobs, pid);
        if (!job) {
            printf("(%d): No such process\n", pid);
            return;
        }
    }

    // Send appropriate signals based on bg or fg command
    if (strcmp(argv[0], "bg") == 0) { // bg command
        job->state = BG;
        printf("[%d] (%d) %s", job->jid, job->pid, job->cmdline);
        kill(-job->pid, SIGCONT); // Send SIGCONT to the process group
    } else if (strcmp(argv[0], "fg") == 0) { // fg command
        job->state = FG;
        kill(-job->pid, SIGCONT); // Send SIGCONT to the process group
        waitfg(job->pid); // Wait for foreground job to finish
    } else {
        printf("Invalid command: %s\n", argv[0]);
        return;
    }
}

/* 
 * waitfg - Block until process pid is no longer the foreground process
 */
void waitfg(pid_t pid) {
    struct job_t *job;

    // Find the job with the given PID
    job = getjobpid(jobs, pid);

    // Wait until the state of the job is not FG
    while (job->state == FG) {
        sleep(1); // Sleep to avoid busy-waiting
    }
    return;
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
void sigchld_handler(int sig) {
    pid_t pid;
    int status;

    while ((pid = waitpid(-1, &status, WNOHANG | WUNTRACED)) > 0) {
        if (WIFEXITED(status)) {
            // Child terminated normally
            deletejob(jobs, pid);
        } else if (WIFSIGNALED(status)) {
            // Child terminated due to uncaught signal
            printf("Job [%d] (%d) terminated by signal %d\n", pid2jid(pid), pid, WTERMSIG(status));
            deletejob(jobs, pid);
        } else if (WIFSTOPPED(status)) {
            // Child stopped by delivery of a signal
            printf("Job [%d] (%d) stopped by signal %d\n", pid2jid(pid), pid, WSTOPSIG(status));
            struct job_t *job = getjobpid(jobs, pid);
            if (job != NULL) {
                job->state = ST;
            }
        }
    }

    if (pid < 0 && errno != ECHILD) {
        unix_error("waitpid error");
    }
    return;
}

/* 
 * sigint_handler - The kernel sends a SIGINT to the shell whenever the
 *    user types ctrl-c at the keyboard.  Catch it and send it along
 *    to the foreground job.  
 */
void sigint_handler(int sig) {
    pid_t fg_pid = fgpid(jobs); // Get the PID of the foreground job
    if (fg_pid != 0) {
        // Send the SIGINT signal to the foreground job process group
        kill(-fg_pid, SIGINT);
    }
    return;
}

/*
 * sigtstp_handler - The kernel sends a SIGTSTP to the shell whenever
 *     the user types ctrl-z at the keyboard. Catch it and suspend the
 *     foreground job by sending it a SIGTSTP.  
 */
void sigtstp_handler(int sig) {
    pid_t fg_pid = fgpid(jobs); // Get the PID of the foreground job
    if (fg_pid != 0) {
        // Send SIGTSTP signal to the foreground job
        if (kill(-fg_pid, SIGTSTP) < 0) {
            unix_error("kill error");
        }
    }
    return;
}

/*
 * sigusr1_handler - child is ready
 */
void sigusr1_handler(int sig) {
    ready = 1;
}


/*********************
 * End signal handlers
 *********************/

/***********************************************
 * Helper routines that manipulate the job list
 **********************************************/

/* clearjob - Clear the entries in a job struct */
void clearjob(struct job_t *job) {
    job->pid = 0;
    job->jid = 0;
    job->state = UNDEF;
    job->cmdline[0] = '\0';
}

/* initjobs - Initialize the job list */
void initjobs(struct job_t *jobs) {
    int i;

    for (i = 0; i < MAXJOBS; i++)
        clearjob(&jobs[i]);
}

/* freejid - Returns smallest free job ID */
int freejid(struct job_t *jobs) {
    int i;
    int taken[MAXJOBS + 1] = {0};
    for (i = 0; i < MAXJOBS; i++)
        if (jobs[i].jid != 0) 
        taken[jobs[i].jid] = 1;
    for (i = 1; i <= MAXJOBS; i++)
        if (!taken[i])
            return i;
    return 0;
}

/* addjob - Add a job to the job list */
int addjob(struct job_t *jobs, pid_t pid, int state, char *cmdline) {
    int i;
    
    if (pid < 1)
        return 0;
    int free = freejid(jobs);
    if (!free) {
        printf("Tried to create too many jobs\n");
        return 0;
    }
    for (i = 0; i < MAXJOBS; i++) {
        if (jobs[i].pid == 0) {
            jobs[i].pid = pid;
            jobs[i].state = state;
            jobs[i].jid = free;
            strcpy(jobs[i].cmdline, cmdline);
            if(verbose){
                printf("Added job [%d] %d %s\n", jobs[i].jid, jobs[i].pid, jobs[i].cmdline);
            }
            return 1;
        }
    }
    return 0; /*suppress compiler warning*/
}

/* deletejob - Delete a job whose PID=pid from the job list */
int deletejob(struct job_t *jobs, pid_t pid) {
    int i;

    if (pid < 1)
        return 0;

    for (i = 0; i < MAXJOBS; i++) {
        if (jobs[i].pid == pid) {
            clearjob(&jobs[i]);
            return 1;
        }
    }
    return 0;
}

/* fgpid - Return PID of current foreground job, 0 if no such job */
pid_t fgpid(struct job_t *jobs) {
    int i;

    for (i = 0; i < MAXJOBS; i++)
        if (jobs[i].state == FG)
            return jobs[i].pid;
    return 0;
}

/* getjobpid  - Find a job (by PID) on the job list */
struct job_t *getjobpid(struct job_t *jobs, pid_t pid) {
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
int pid2jid(pid_t pid) {
    int i;

    if (pid < 1)
        return 0;
    for (i = 0; i < MAXJOBS; i++)
        if (jobs[i].pid == pid) {
            return jobs[i].jid;
    }
    return 0;
}

/* listjobs - Print the job list */
void listjobs(struct job_t *jobs) {
    int i;
    
    for (i = 0; i < MAXJOBS; i++) {
        if (jobs[i].pid != 0) {
            printf("[%d] (%d) ", jobs[i].jid, jobs[i].pid);
            switch (jobs[i].state) {
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
 * usage - print a help message and terminate
 */
void usage(void) {
    printf("Usagelol: shell [-hvp]\n");
    printf("   -h   print this message\n");
    printf("   -v   print additional diagnostic information\n");
    printf("   -p   do not emit a command prompt\n");
    exit(1);
}

/*
 * unix_error - unix-style error routine
 */
void unix_error(char *msg) {
    fprintf(stdout, "%s: %s\n", msg, strerror(errno));
    exit(1);
}

/*
 * app_error - application-style error routine
 */
void app_error(char *msg) {
    fprintf(stdout, "%s\n", msg);
    exit(1);
}

/*
 * Signal - wrapper for the sigaction function
 */
handler_t *Signal(int signum, handler_t *handler) {
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
void sigquit_handler(int sig) {
    printf("Terminating after receipt of SIGQUIT signal\n");
    exit(1);
}

//Code generated by ChatGPT and edited by group members.
