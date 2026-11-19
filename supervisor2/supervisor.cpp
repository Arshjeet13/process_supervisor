// GOAL
// 1 parent process. It should spawn N child processes
// It should be in perpetual sleep, should be able to be woken up by any signal. Hence, we use epoll

#include <iostream>
#include <unistd.h>
#include <map>
#include <sys/wait.h>
#include <sys/signalfd.h>
#include <sys/epoll.h>
#include <string.h>
#include <cerrno>
#include <signal.h>
#include <string>
#include <vector>

const int MAX_WORKERS = 1024;

// Set up in main(). Workers inherit all three through fork(), so spawn_worker
// uses them to undo that in the child
sigset_t original_mask; // signal mask from before we blocked anything
int sfd  = -1;          // signalfd
int epfd = -1;          // epoll fd

void shutdown(std::vector<pid_t>& workers, int N, int exitcode);

/**
 * Worker goes into an infinite loop of printing its pid
 */
void worker(int slot){
    //worker <slot> pid=<pid> started
    pid_t pid = getpid();
    std::cout << "worker " << slot << " pid=" << pid << " started\n" << std::flush;
    while(true){
        std::cout << "worker " << slot << " pid=" << pid << "\n" << std::flush;
        sleep(1);
    }
}

/**
 * Creates a worker via forking a child process
 * Child goes into worker() method. If worker method ever returns, child dies via _exit(0);
 * Parent adds pid in correct slot in workers and returns from this function
 */
void spawn_worker(int slot, std::vector<pid_t>& workers){
    pid_t p = fork();

    if(p < 0){
        // We are still the parent here, so clean up the workers that do exist
        std::cerr << "fork (slot " << slot << ") : " << strerror(errno) << "\n";
        shutdown(workers, workers.size(), 1);
    }
    if(p == 0){
        // The child inherited the parent's blocked mask. Without restoring it,
        // shutdown's SIGTERM would stay pending forever and waitpid would hang
        sigprocmask(SIG_SETMASK, &original_mask, NULL);
        // Only the parent reads these
        close(sfd);
        close(epfd);
        worker(slot);
        _exit(0);
    }
    if(p > 0){
        workers[slot] = p;
    }
}

/**
 * Blocks SIGCHLD, SIGTERM and SIGINT. Now, they remain pending
 * until they are explicitly read by our program (through the signalfd).
 * Saves the previous mask in original_mask, for workers to restore.
 */
void block_signals(){
    sigset_t block_set;
    sigemptyset(&block_set);
    sigaddset(&block_set, SIGCHLD);
    sigaddset(&block_set, SIGTERM);
    sigaddset(&block_set, SIGINT );

    if(sigprocmask(SIG_BLOCK, &block_set, &original_mask) != 0){
        std::cerr << "sigprocmask : " << strerror(errno) << "\n";
        exit(1);
    }
}

/**
 * Watches: SIGCHLD, SIGTERM, SIGINT (does not block them; block_sigchld() does that)
 * Since the above signals are blocked, one of them when sent to our process becomes pending.
 * This function returns a fd that allows us to read these pending signals
 */
int get_signalfd(){
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGCHLD);
    sigaddset(&mask, SIGTERM);
    sigaddset(&mask, SIGINT );

    int sfd = signalfd(-1, &mask, 0);
    if(sfd == -1){
        std::cerr << "signalfd : " << strerror(errno) << "\n";
        exit(1);
    }
    return sfd;
}

/**
 * Reports whether worked died via exiting or killed by a signal
 */
void report_death(int wstatus, int slot, pid_t pid){
    if(WIFEXITED(wstatus)){
        // slot <k> pid=<pid> died: exited code=<c>
        std::cout << "slot " << slot << " pid=" << pid << " died: exited code=" << WEXITSTATUS(wstatus) << "\n" << std::flush;
    }
    if(WIFSIGNALED(wstatus)){
        // slot <k> pid=<pid> died: killed by signal <s> (<name>)
        const char* abbrev = sigabbrev_np(WTERMSIG(wstatus));
        std::string signal_name = (abbrev != nullptr) ? abbrev : "unknown";
        std::cout << "slot " << slot << " pid=" << pid << " died: killed by signal " << WTERMSIG(wstatus) << " (" << signal_name << ")" <<"\n" << std::flush;
    }
}

void shutdown(std::vector<pid_t>& workers, int N, int exitcode){
    for(int i=0; i<N; ++i){
        pid_t pid = workers[i];
        // Slot never filled (fork failed during startup). kill(0, ...) would signal
        // our whole process group, including us, so skip it
        if(pid <= 0) continue;
        kill(pid, SIGTERM); // Graceful stop
        waitpid(pid, NULL, 0); // Clean up process table entry
    }
    exit(exitcode);
}

int main(int argc, char** argv){
    if(argc != 2){
        std::cerr << "Usage: "<< argv[0] << " <num_workers>\n";
        exit(1);
    }

    // stoi throws on non-numbers and out-of-range values, and stops at the first
    // non-digit ("12abc" -> 12), so check that the whole argument was consumed
    int N = 0;
    try{
        size_t parsed = 0;
        N = std::stoi(argv[1], &parsed);
        if(argv[1][parsed] != '\0') throw std::invalid_argument("trailing characters");
    }
    catch(const std::exception&){
        std::cerr << "num_workers must be a whole number, got \"" << argv[1] << "\"\n";
        exit(1);
    }
    if(N < 1 || N > MAX_WORKERS){
        std::cerr << "num_workers must be between 1 and " << MAX_WORKERS << ", got " << N << "\n";
        exit(1);
    }

    // We block SIGCHILD before spawning the workers to ensure that SIGCHLD doesn't
    // get delivered directly to its default action (discard). Blocking it keeps it
    // pending so that it can be read from our signal fd
    block_signals();

    // sfd is the fd where SIGCHLD, SIGTERM and SIGINT can be read from
    sfd = get_signalfd();

    // epfd is the epoll's fd
    epfd = epoll_create1(0);
    if(epfd == -1){
        std::cerr << "epoll_create1 : " << strerror(errno) << "\n";
        exit(1);
    }

    struct epoll_event epevent{};
    epevent.events  = EPOLLIN;
    epevent.data.fd = sfd;

    if(epoll_ctl(epfd, EPOLL_CTL_ADD, sfd, &epevent) != 0){
        std::cerr << "epoll_ctl : " << strerror(errno) << "\n";
        exit(1);
    }

    std::vector<pid_t> workers(N);

    for(int i=0; i<N; ++i){
        spawn_worker(i, workers);
    }

    while(true){
        struct epoll_event events[1];
        int nfds = epoll_wait(epfd, events, 1, -1);
        if(nfds == -1){
            std::cerr << "epoll_wait : " << strerror(errno) << "\n";
            shutdown(workers, N, 1);
        }
        else{
            struct signalfd_siginfo siginfo;
            if(read(sfd, &siginfo, sizeof(signalfd_siginfo)) != sizeof(signalfd_siginfo)){
                std::cerr << "read from signalfd : " << strerror(errno) << "\n";
                shutdown(workers, N, 1);
            }

            if(siginfo.ssi_signo == SIGTERM || siginfo.ssi_signo == SIGINT){
                std::cout << "parent: got signal " << siginfo.ssi_signo << ", shutting down\n" << std::flush;
                shutdown(workers, N, 0);
            }

            while(true){
                int wstatus;
                pid_t pid = waitpid((pid_t) -1, &wstatus, WNOHANG);
                if(pid == 0){
                    break;
                }
                if(pid == -1){
                    if(errno != ECHILD){
                        std::cerr << "waitpid : " << strerror(errno) << "\n";
                        shutdown(workers, N, 1);
                    }
                    else{
                        for(int i=0; i<N; ++i){
                            spawn_worker(i, workers);
                        }
                        continue;
                    }
                }

                int slot = -1;
                for(int i=0; i < N; ++i){
                    if(workers[i] == pid){
                        slot = i;
                        break;
                    }
                }                

                if(slot == -1){
                    std::cerr << "No slot with pid " << pid << " exists in workers\n";
                    shutdown(workers, N, 1);
                }

                report_death(wstatus, slot, pid);

                spawn_worker(slot, workers);
            }            
        }
    }
}