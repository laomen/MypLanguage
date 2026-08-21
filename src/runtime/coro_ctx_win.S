/* src/runtime/coro_ctx_win.S — Windows x64 (Win64 ABI) 寄存器级协程上下文切换
 *
 * 对应 Linux 版 coro_ctx.S（SysV）。Win64 差异：
 *   - 前两个参数在 rcx / rdx（不是 rdi / rsi）。
 *   - 非易失（callee-saved）寄存器：rbx, rbp, rsi, rdi, r12-r15
 *     + xmm6..xmm15（10 个，每个 16 字节）——协程内大量使用 double，必须保存，
 *     否则切换后浮点寄存器被破坏。
 *
 * C 签名:
 *   void myp_ctx_switch(myp_ctx_t* save, myp_ctx_t* load);
 * myp_ctx_t = { void* rsp; }，rsp 指向保存块（256 字节对齐基址，有效 248 字节）：
 *   [ base+0:   rbp
 *     base+8:   rbx
 *     base+16:  r12
 *     base+24:  r13
 *     base+32:  r14
 *     base+40:  r15
 *     base+48:  rsi   (Win64 callee-saved)
 *     base+56:  rdi   (Win64 callee-saved)
 *     base+64:  rip   (恢复地址 / 首次进入的 entry)
 *     base+80..240:   xmm6..xmm15 (10×16B，16 对齐 → movaps 可用)
 *     base+248: 入口 rsp = base+248（%16==8，满足 Win64 函数入口对齐）]
 *
 * 入口栈对齐：init 设 base = top-256（top 16 对齐 → base%16==0）；恢复时
 * addq $248 → rsp = base+248，%16==8。movaps 要求 [rsp+80] 16 对齐：入口
 * rsp%16==8，subq $248 后 %16==0，80(%rsp) 16 对齐。
 */
    .text
    .globl  myp_ctx_switch
myp_ctx_switch:
    /* rcx = save, rdx = load */
    subq    $248, %rsp
    /* 整数非易失寄存器（Win64） */
    movq    %rbp, 0(%rsp)
    movq    %rbx, 8(%rsp)
    movq    %r12, 16(%rsp)
    movq    %r13, 24(%rsp)
    movq    %r14, 32(%rsp)
    movq    %r15, 40(%rsp)
    movq    %rsi, 48(%rsp)
    movq    %rdi, 56(%rsp)
    leaq    1f(%rip), %rax          /* 恢复地址 */
    movq    %rax, 64(%rsp)
    /* 浮点非易失寄存器 xmm6..xmm15 */
    movaps  %xmm6, 80(%rsp)
    movaps  %xmm7, 96(%rsp)
    movaps  %xmm8, 112(%rsp)
    movaps  %xmm9, 128(%rsp)
    movaps  %xmm10, 144(%rsp)
    movaps  %xmm11, 160(%rsp)
    movaps  %xmm12, 176(%rsp)
    movaps  %xmm13, 192(%rsp)
    movaps  %xmm14, 208(%rsp)
    movaps  %xmm15, 224(%rsp)
    movq    %rsp, 0(%rcx)           /* save->rsp = 保存块 */
    movq    0(%rdx), %rsp           /* 切换到 load->rsp */
    /* 恢复（对称） */
    movaps  80(%rsp), %xmm6
    movaps  96(%rsp), %xmm7
    movaps  112(%rsp), %xmm8
    movaps  128(%rsp), %xmm9
    movaps  144(%rsp), %xmm10
    movaps  160(%rsp), %xmm11
    movaps  176(%rsp), %xmm12
    movaps  192(%rsp), %xmm13
    movaps  208(%rsp), %xmm14
    movaps  224(%rsp), %xmm15
    movq    64(%rsp), %rax          /* rip：恢复地址 或 初始 entry */
    movq    56(%rsp), %rdi
    movq    48(%rsp), %rsi
    movq    40(%rsp), %r15
    movq    32(%rsp), %r14
    movq    24(%rsp), %r13
    movq    16(%rsp), %r12
    movq    8(%rsp), %rbx
    movq    0(%rsp), %rbp
    addq    $248, %rsp
    jmpq    *%rax
1:
    ret
