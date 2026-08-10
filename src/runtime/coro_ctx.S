/* src/runtime/coro_ctx.S — x86-64 SysV 寄存器级协程上下文切换
 *
 * 替代 ucontext 的 swapcontext（后者每次切换都做 sigprocmask syscall，~200ns）。
 * 本实现只保存/恢复 callee-saved 寄存器 + rsp/rip（正是 C 函数级切换所需），
 * 无 syscall、无信号掩码操作，单次切换 ~20-40ns。
 *
 * C 签名:
 *   void myp_ctx_switch(myp_ctx_t* save, myp_ctx_t* load);
 * 其中 myp_ctx_t 即 { void* rsp; }——指向如下保存块：
 *   [ rsp ->  rip(恢复地址) , r15, r14, r13, r12, rbx, rbp ]
 *
 * 语义（对称于 swapcontext）:
 *   把当前上下文存入 *save；切换到 *load。每个上下文被再次切换回时，
 *   "从调用 myp_ctx_switch 的地方返回"。首次进入一个新纤程栈时，
 *   load->rsp 指向 __myp_coro_trampoline 的入口帧（见 myp_ctx_init）。
 *
 * 兼容: GNU as 与 clang 集成汇编器；Linux x86-64（SysV）。
 */
    .text
    .globl  myp_ctx_switch
    .type   myp_ctx_switch, @function
myp_ctx_switch:
    /* 入参: rdi = save, rsi = load */
    pushq   %rbp
    pushq   %rbx
    pushq   %r12
    pushq   %r13
    pushq   %r14
    pushq   %r15
    leaq    1f(%rip), %rax          /* 恢复地址：本函数的 1: ret */
    pushq   %rax
    movq    %rsp, 0(%rdi)           /* save->rsp = 当前栈（指向保存块） */
    movq    0(%rsi), %rsp           /* 切换到 load->rsp */
    popq    %rax                    /* rip（恢复地址 或 初始入口帧的 entry） */
    popq    %r15
    popq    %r14
    popq    %r13
    popq    %r12
    popq    %rbx
    popq    %rbp
    jmpq    *%rax
1:
    ret                             /* 从原调用者处返回 */
    .size   myp_ctx_switch, .-myp_ctx_switch

    /* 标记栈区不可执行（现代工具链告警消除） */
    .section .note.GNU-stack,"",@progbits
