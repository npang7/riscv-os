#include "syscall.h"

void main(void) {
    int i;
    int pid;
    _print("This is the main thred.\n");

    // 每个进程只 fork 一次，形成一个链式结构
    for (i = 1; i < 16; i++) {
        pid = _fork();

        if (pid == 0) {
            if (i == 1) {
                _print("Successfully fork child process 1.\n");
            }
            if (i == 2) {
                _print("Successfully fork child process 2.\n");
            }
            if (i == 3) {
                _print("Successfully fork child process 3.\n");
            }
            if (i == 4) {
                _print("Successfully fork child process 4.\n");
            }
            if (i == 5) {
                _print("Successfully fork child process 5.\n");
            }
            if (i == 6) {
                _print("Successfully fork child process 6.\n");
            }
            if (i == 7) {
                _print("Successfully fork child process 7.\n");
            }
            if (i == 8) {
                _print("Successfully fork child process 8.\n");
            }
            if (i == 9) {
                _print("Successfully fork child process 9.\n");
            }
            if (i == 10) {
                _print("Successfully fork child process 10.\n");
            }
            if (i == 11) {
                _print("Successfully fork child process 11.\n");
            }
            if (i == 12) {
                _print("Successfully fork child process 12.\n");
            }
            if (i == 13) {
                _print("Successfully fork child process 13.\n");
            }
            if (i == 14) {
                _print("Successfully fork child process 14.\n");
            }
            if (i == 15) {
                _print("Successfully fork child process 15.\n");
            }

            continue;
        }
        else if (pid > 0) {
            // 父进程等待它刚刚 fork 的子进程退出
            _wait(pid);
            break;  // 父进程退出循环，不再创建新子进程
        }
        else {
            _print("fork failed\n");
            _exit();
        }
    }

    _exit();
}
