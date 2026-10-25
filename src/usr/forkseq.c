#include "syscall.h"

void main(void) {
    int pid;
    int i;

    for (i = 0; i < 15; i++) {
        pid = _fork();

        // 进入子进程
        if (pid == 0) {
            _print("Child process created.\n");
            _exit();
        }

        // 在父进程中，记录该子进程的 pid
        // 父进程等待这个刚刚创建的子进程退出
        _wait(pid);
        if (pid == 1) {
            _print("child process 1 exited.\n");
        }
        if (pid == 2) {
            _print("child process 2 exited.\n");
        }
        if (pid == 3) {
            _print("child process 3 exited.\n");
        }
        if (pid == 4) {
            _print("child process 4 exited.\n");
        }
        if (pid == 5) {
            _print("child process 5 exited.\n");
        }
        if (pid == 6) {
            _print("child process 6 exited.\n");
        }
        if (pid == 7) {
            _print("child process 7 exited.\n");
        }
        if (pid == 8) {
            _print("child process 8 exited.\n");
        }
        if (pid == 9) {
            _print("child process 9 exited.\n");
        }
        if (pid == 10) {
            _print("child process 10 exited.\n");
        }
        if (pid == 11) {
            _print("child process 11 exited.\n");
        }
        if (pid == 12) {
            _print("child process 12 exited.\n");
        }
        if (pid == 13) {
            _print("child process 13 exited.\n");
        }
        if (pid == 14) {
            _print("child process 14 exited.\n");
        }
        if (pid == 15) {
            _print("child process 15 exited.\n");
        }
        
    }

    _print("Sequential forking test completed.\n");
    _exit();
}
