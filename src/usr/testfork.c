/*#include "syscall.h"
#include "string.h"

void main(void) {
    int result;

    _fork();
    
    result = _fsopen(3, "trek");
    
    if (result < 0) {
        _print("_fsopen failed");
        _exit();
    }

    _exec(3, 0, NULL);
}*/
#include "syscall.h"
#include "string.h"

void main(void) {
    int result;

    if (_fork()) {
        // exec fibonacci
        _print("111\n");
        result = _fsopen(4, "fib");

        if (result < 0) {
            _print("_fsopen failed");
            _exit();
        }

        _exec(4, 0, NULL);
        
    
    } else {
        // // Open ser1 device as fd=0
        _print("222\n");
        /*result = _devopen(0, "uart", 1);

        if (result < 0) {
            _print("_devopen failed");
            _exit();
        }*/

        // exec rule30

        result = _fsopen(5, "fib");

        if (result < 0) {
            _print("_fsopen failed");
            _exit();
        }

        _exec(5, 0, NULL);
        
        
    }
}