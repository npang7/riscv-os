#include "syscall.h"

void fork_and_wait_round() {
    //int child_pids[16];
    int i;

    _print("Start forking 16 processes...\n");

    for (i = 1; i < 16; i++) {
        int pid = _fork();
        if (pid < 0) {
            _print("fork failed.\n");
            _exit();
        }
        if (pid == 0) {
            _print("Child process created.\n");
            _exit();
        }
        //child_pids[i] = pid;
    }

    for (i = 1; i < 16; i++) {
        //_wait(child_pids[i]);
        _wait(i);
        _print("Collected child processes.\n");
    }

    _print("complete.\n");
}

void main(void) {
    _print("Test: Fork 16 -> Exit 16 -> Fork 16 again\n");

    fork_and_wait_round();  // Round 1: fork + wait 16
    fork_and_wait_round();  // Round 2: fork + wait 16 again

    _print("Test complete: All 32 processes forked, exited, and collected.\n");
    _exit();
}
