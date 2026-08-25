# freestanding 入口：无 CRT、无 libc。_start → main → exit(返回值)。
# main 内部已自行 exit(0)（见 hello_fs.myp），这里只是兜底收尾。
    .global _start
    .text
_start:
    call main
    mov %eax, %edi        # main() 返回 int → exit 状态
    mov $60, %eax         # exit syscall
    syscall
    hlt                   # 永不返回；防御性停住
    .size _start, .-_start
