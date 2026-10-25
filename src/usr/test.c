#include "syscall.h"
#include "string.h"

void main(void) {
    int result;

    result = _fscreate("hello");

    if (result < 0) {
        _print("_fscreate failed");
        _exit();
    }

    result = _fsopen(1, "hello");

    if (result < 0) {
        _print("_fsopen failed");
        _exit();
    }

    result = _close(1);

    if (result < 0) {
        _print("_close failed");
        _exit();
    }

    result = _fsopen(1, "hello");

    if (result < 0) {
        _print("_fsopen failed");
        _exit();
    }

    char str[513];
    for (int i = 0; i < 512; i++) {
        str[i] = 'a';
    }

    result = _write(1, str, 512);

    if (result < 0) {
        _print("_write failed");
        _close(1);
        _exit();
    }

    // test GETBLKSZ
    if (_ioctl(1, 0, NULL) == 1) {
        _print("\nioctl succeeds\n");
    }

    result = _close(1);

    if (result < 0) {
        _print("_close failed");
        _exit();
    }

    result = _fsopen(1, "empty");

    if (result < 0) {
        _print("_fsopen failed");
        _exit();
    }

    char str2[5];
    result = _read(1, str2, 4);

    if (result < 0) {
        _print("_read failed");
        _close(1);
        _exit();
    }
    if (result == 4) {
        _print("\n_read succeeds\n");
    }
    _usleep(2000000);
    _print(str2);

    result = _fsdelete("empty");

    if (result < 0) {
        _print("_fsdelete failed");
        _exit();
    }

    _devopen(2, "uart", 1);
    _fsopen(3, "trek");
    _exec(3, 0, NULL);

    _exit();
}

