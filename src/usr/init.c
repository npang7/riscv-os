#include <stddef.h>
#include "syscall.h"

void main(void) {
    char * argv[] = { "trek_cp2", NULL };
    int result;
    int k = 0;

    // Workaround for _wait(0) in main thread not returning -ECHILD because of
    // kernel threads
    if (_wait(_fork()) > 0) {
        //return;
        _devopen(2, "uart", 1);
        _fsopen(3, argv[0]);
        _exec(3, 1, argv);
        _exit(); // fallback if exec fails
    }
    
    _iodup(0, 1); // output is nullio also
    int r = _fsopen(3, argv[0]);
    if (r < 0) {
        _print("fsopen failed");
    }

    // Attach shell to each uart except uart0
    for (;;) {
        result = _devopen(2, "uart", 2);
        if (result < 0)
            break;
        
        if (_fork() == 0) {
            _print("Executing on UART");
            _exec(3, 1, argv);
            _print("_exec failed");
        }
        _close(2);
    }

    _close(3);

    while (_wait(0) > 0)
        continue;
}
