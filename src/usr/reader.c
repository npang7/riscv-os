// reader.c
#include "syscall.h"
#include "string.h"

int main() {
    char buf[128];
    long n;

    while ((n = _read(0, buf, sizeof(buf))) > 0) {
        _write(1, buf, n);
    }

    _exit();  // 永不返回
}
