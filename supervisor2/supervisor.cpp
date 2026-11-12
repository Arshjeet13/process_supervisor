// GOAL
// 1 parent process. It should spawn N child processes
// It should be in perpetual sleep, should be able to be woken up by any signal. Hence, we use epoll

#include <iostream>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/signalfd.h>
#include <string.h>
#include <cerrno>
#include <signal.h>
#include <vector>

void worker(int slot){
    //worker <slot> pid=<pid> started
    pid_t pid = getpid();
    std::cout << "worker " << slot << " pid=" << pid << " started\n" << std::flush;
    while(true){
        std::cout << "worker " << slot << " pid=" << pid << "\n" << std::flush;
        sleep(1);
    }
}

void spawn_worker(int slot, std::vector<pid_t>& workers){
    pid_t p = fork();

    if(p < 0){
        std::cerr << "Fork failed\n" << std::flush;
        exit(1);
    }
    if(p == 0){
        worker(slot);
        _exit(0);
    }
    if(p > 0){
        workers[slot] = p;
    }
}

void block_sigchld(){
    sigset_t block_set, old_set;
    sigemptyset(&block_set);
    sigaddset(&block_set, SIGCHLD);

    if(sigprocmask(SIG_BLOCK, &block_set, &old_set) != 0){
        std::cerr << "Blocking SIGCHLD failed\n";
        exit(1);
    }
}

int get_signalfd(){
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGCHLD);
    return signalfd(-1, &mask, 0);
}

int main(int argc, char** argv){
    if(argc != 2){
        std::cerr << "Usage: "<< argv[0] << " <arg1>\n";
        exit(1);
    }

    int N = std::stoi(argv[1]);

    std::vector<pid_t> workers(N);

    block_sigchld();
    int fd = get_signalfd();

    for(int i=0; i<N; ++i){
        spawn_worker(i, workers);
    }    

    
    
}