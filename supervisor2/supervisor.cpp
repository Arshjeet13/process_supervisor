// GOAL
// 1 parent process. It should spawn N child processes
// It should be in perpetual sleep, should be able to be woken up by any signal. Hence, we use epoll

#include <iostream>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/signalfd.h>
#include <sys/epoll.h>
#include <string.h>
#include <cerrno>
#include <signal.h>
#include <vector>

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
        std::cerr << "Fork failed\n";
        _exit(1);
    }
    if(p == 0){
        worker(slot);
        _exit(0);
    }
    if(p > 0){
        workers[slot] = p;
    }
}

/**
 * Gives the signal SIGCHLD a blocking property. Now, it remains pending
 * until it is explicitly read by our program 
 */
void block_sigchld(){
    sigset_t block_set, old_set;
    sigemptyset(&block_set);
    sigaddset(&block_set, SIGCHLD);

    if(sigprocmask(SIG_BLOCK, &block_set, &old_set) != 0){
        std::cerr << "Blocking SIGCHLD failed\n";
        exit(1);
    }
}

/**
 * Since SIGCHLD is blocked, a SIGCHLD signal sent to our process becomes pending.
 * This function returns a fd that allows us to read these pending signals
 */
int get_signalfd(){
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGCHLD);
    int sfd = signalfd(-1, &mask, 0);
    if(sfd == -1){
        std::cerr << "signalfd : " << strerror(errno) << "\n";
        exit(1);
    }
    return sfd;
}

int main(int argc, char** argv){
    if(argc != 2){
        std::cerr << "Usage: "<< argv[0] << " <arg1>\n";
        exit(1);
    }
    int N = std::stoi(argv[1]);

    // We block SIGCHILD before spawning the workers to ensure that SIGCHLD doesn't
    // get delivered directly to its default action (discard). Blocking it keeps it
    // pending so that it can be read from our signal fd
    block_sigchld();

    // sfd is the fd where SIGCHLD signals get rerouted to
    int sfd = get_signalfd();

    // epfd is the epoll's fd
    int epfd = epoll_create1(0);

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

    // while(true){
    //     struct epoll_event events[1];
    //     int nfds = epoll_wait(epfd, events, 1, -1);
    //     if(nfds == -1){
    //         std::cerr << "epoll_wait : " << strerror(errno) << "\n";
    //     }
    // }
}