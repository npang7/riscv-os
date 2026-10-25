#include "console.h"
#include "heap.h"
#include "io.h"
#include "string.h"
#include "assert.h"
#include "elf.h"

// 外部符号，由链接器提供
extern char _kimg_end[];

// 把 ELF 文件用 objcopy 工具转换成 C 字节数组 //手动放一个小 ELF 文件到 elf_binary[]
// 假设 elf_binary 是一个合法的 ELF 文件
extern char _binary_usr_bin_trek_start[];
extern char _binary_usr_bin_trek_end[];

void main(void) {
    heap_init(_kimg_end, _kimg_end + 0x20000); // for memory_alloc_page()
    console_init();

    kprintf("== Running elftest ==\n");

    // 构造 elf binary 在内存中的起止
    void *elf_data = _binary_usr_bin_trek_start;
    size_t elf_size = _binary_usr_bin_trek_end - _binary_usr_bin_trek_start;

    // 1. 创建一个 memory io
    struct io * elfio = create_memory_io(elf_data, elf_size); //模拟io接口
    assert(elfio);

    // 2. 调用 elf_load
    void (*entry)(void);
    int ret = elf_load(elfio, &entry);
    assert(ret == 0);

    kprintf(" ELF load succeeded. Entry at: %p\n", entry);

    // 3. 如果你想测试运行（非必须）
    // entry();  // 可选：真的调用 elf main 函数（需准备环境）

    ioclose(elfio);
    kprintf("== elftest finished ==\n");


}
