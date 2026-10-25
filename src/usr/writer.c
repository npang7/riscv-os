// prog2.c
#include "syscall.h"
#include "string.h"

void int_to_str(int n, char *buf) {
    // 转换整数为字符串（简单实现）
    char tmp[16];
    int i = 0;
    if (n == 0) {
        buf[0] = '0'; buf[1] = 0;
        return;
    }
    while (n > 0) {
        tmp[i++] = '0' + (n % 10);
        n /= 10;
    }
    // 逆序复制
    for (int j = 0; j < i; j++) {
        buf[j] = tmp[i - j - 1];
    }
    buf[i] = 0;
}

int main() {
    char buf[128];
    long n;
    int total = 0;

    while ((n = _read(0, buf, sizeof(buf))) > 0) {
        total += n;
    }

    char msg[64] = "Total characters: ";
    char numbuf[16];
    int_to_str(total, numbuf);

    // 拼接字符串
    char *p = msg;
    while (*p) p++;  // 定位到末尾
    char *q = numbuf;
    while (*q) *(p++) = *(q++);
    *(p++) = '\n';
    *p = 0;

    _write(1, msg, p - msg);
    _exit();
}
