#include "syscall.h" 

void main(void) { 
    int parent; 
    int trekFd; 
    trekFd = _fsopen(-1, "trek_cp2");
	parent = _fork();
    if (parent) {
        int r = _devopen(2, "uart", 3); 
        if (r < 0) {
            _print("parent devopen failed\n");
        }
        int ret = _exec(trekFd, 0, NULL);
        if (ret < 0) {
            _print("parent exec failed\n");
        }
    } else {
        int r2 = _devopen(2, "uart", 4);
        if (r2 < 0) {
            _print("child devopen failed\n");
        }
        int ret2 = _exec(trekFd, 0, NULL);
        if (ret2 < 0) {
            _print("child exec failed\n");
        }
    }
	return;
}
