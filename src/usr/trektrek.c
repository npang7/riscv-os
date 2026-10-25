#include "syscall.h"

void main(void) {
    int parent;
    int trekFd;
    _devopen(2, "uart", 1);
    parent = _fork();
    if (parent) {
        trekFd = _fsopen(-1, "trek");
        if (trekFd < 0) {
            _print("parent: failed to open trek");
            return;
        }
        _exec(trekFd, 0, NULL);
        _print("parent: exec failed with code");
    } else {
        trekFd = _fsopen(-1, "trek_cp2");
        if (trekFd < 0) {
            _print("child: failed to open trek");
            return;
        }
        _exec(trekFd, 0, NULL);
        _print("child: exec failed with code");
    }
}
