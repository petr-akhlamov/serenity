/*
 * Copyright (c) 2018-2020, Andreas Kling <kling@serenityos.org>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice, this
 *    list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <Kernel/Arch/i386/CPU.h>
#include <Kernel/Process.h>
#include <Kernel/Random.h>
#include <Kernel/Syscall.h>
#include <Kernel/ThreadTracer.h>
#include <Kernel/VM/MemoryManager.h>

namespace Kernel {

extern "C" void syscall_handler(TrapFrame*);
extern "C" void syscall_asm_entry();

asm(
    ".globl syscall_asm_entry\n"
    "syscall_asm_entry:\n"
    "    pushl $0x0\n"
    "    pusha\n"
    "    pushl %ds\n"
    "    pushl %es\n"
    "    pushl %fs\n"
    "    pushl %gs\n"
    "    pushl %ss\n"
    "    mov $" __STRINGIFY(GDT_SELECTOR_DATA0) ", %ax\n"
    "    mov %ax, %ds\n"
    "    mov %ax, %es\n"
    "    mov $" __STRINGIFY(GDT_SELECTOR_PROC) ", %ax\n"
    "    mov %ax, %fs\n"
    "    cld\n"
    "    xor %esi, %esi\n"
    "    xor %edi, %edi\n"
    "    pushl %esp \n" // set TrapFrame::regs
    "    subl $" __STRINGIFY(TRAP_FRAME_SIZE - 4) ", %esp \n"
    "    movl %esp, %ebx \n"
    "    pushl %ebx \n" // push pointer to TrapFrame
    "    call enter_trap_no_irq \n"
    "    movl %ebx, 0(%esp) \n" // push pointer to TrapFrame
    "    call syscall_handler \n"
    "    movl %ebx, 0(%esp) \n" // push pointer to TrapFrame
    "    jmp common_trap_exit \n");

namespace Syscall {

static int handle(RegisterState&, u32 function, u32 arg1, u32 arg2, u32 arg3);

void initialize()
{
    register_user_callable_interrupt_handler(syscall_vector, syscall_asm_entry);
    klog() << "Syscall: int 0x82 handler installed";
}

#pragma GCC diagnostic ignored "-Wcast-function-type"
typedef int (Process::*Handler)(u32, u32, u32);
#define __ENUMERATE_REMOVED_SYSCALL(x) nullptr,
#define __ENUMERATE_SYSCALL(x) reinterpret_cast<Handler>(&Process::sys$##x),
static Handler s_syscall_table[] = {
    ENUMERATE_SYSCALLS
};
#undef __ENUMERATE_SYSCALL
#undef __ENUMERATE_REMOVED_SYSCALL

int handle(RegisterState& regs, u32 function, u32 arg1, u32 arg2, u32 arg3)
{
    ASSERT_INTERRUPTS_ENABLED();
    auto current_thread = Thread::current();
    auto& process = current_thread->process();
    current_thread->did_syscall();

    if (function == SC_exit || function == SC_exit_thread) {
        // These syscalls need special handling since they never return to the caller.
        cli();
        if (function == SC_exit)
            process.sys$exit((int)arg1);
        else
            process.sys$exit_thread((void*)arg1);
        ASSERT_NOT_REACHED();
        return 0;
    }

    if (function == SC_fork)
        return process.sys$fork(regs);

    if (function == SC_sigreturn)
        return process.sys$sigreturn(regs);

    if (function >= Function::__Count) {
        dbg() << process << ": Unknown syscall %u requested (" << arg1 << ", " << arg2 << ", " << arg3 << ")";
        return -ENOSYS;
    }

    if (s_syscall_table[function] == nullptr) {
        dbg() << process << ": Null syscall " << function << " requested: \"" << to_string((Function)function) << "\", you probably need to rebuild this program.";
        return -ENOSYS;
    }
    return (process.*(s_syscall_table[function]))(arg1, arg2, arg3);
}

}

void syscall_handler(TrapFrame* trap)
{
    auto& regs = *trap->regs;
    // Special handling of the "gettid" syscall since it's extremely hot.
    // FIXME: Remove this hack once userspace locks stop calling it so damn much.
    auto current_thread = Thread::current();
    auto& process = current_thread->process();
    if (regs.eax == SC_gettid) {
        regs.eax = process.sys$gettid();
        current_thread->did_syscall();
        return;
    }

    if (current_thread->tracer() && current_thread->tracer()->is_tracing_syscalls()) {
        current_thread->tracer()->set_trace_syscalls(false);
        current_thread->tracer_trap(regs);
    }

    // Make sure SMAP protection is enabled on syscall entry.
    clac();

    // Apply a random offset in the range 0-255 to the stack pointer,
    // to make kernel stacks a bit less deterministic.
    auto* ptr = (char*)__builtin_alloca(get_fast_random<u8>());
    asm volatile(""
                 : "=m"(*ptr));

    if (!MM.validate_user_stack(process, VirtualAddress(regs.userspace_esp))) {
        dbg() << "Invalid stack pointer: " << String::format("%p", regs.userspace_esp);
        handle_crash(regs, "Bad stack on syscall entry", SIGSTKFLT);
        ASSERT_NOT_REACHED();
    }

    auto* calling_region = MM.region_from_vaddr(process, VirtualAddress(regs.eip));
    if (!calling_region) {
        dbg() << "Syscall from " << String::format("%p", regs.eip) << " which has no region";
        handle_crash(regs, "Syscall from unknown region", SIGSEGV);
        ASSERT_NOT_REACHED();
    }

    if (calling_region->is_writable()) {
        dbg() << "Syscall from writable memory at " << String::format("%p", regs.eip);
        handle_crash(regs, "Syscall from writable memory", SIGSEGV);
        ASSERT_NOT_REACHED();
    }

    process.big_lock().lock();
    u32 function = regs.eax;
    u32 arg1 = regs.edx;
    u32 arg2 = regs.ecx;
    u32 arg3 = regs.ebx;
    regs.eax = (u32)Syscall::handle(regs, function, arg1, arg2, arg3);

    if (current_thread->tracer() && current_thread->tracer()->is_tracing_syscalls()) {
        current_thread->tracer()->set_trace_syscalls(false);
        current_thread->tracer_trap(regs);
    }

    process.big_lock().unlock();

    // Check if we're supposed to return to userspace or just die.
    current_thread->die_if_needed();

    if (current_thread->has_unmasked_pending_signals())
        (void)current_thread->block<Thread::SemiPermanentBlocker>(Thread::SemiPermanentBlocker::Reason::Signal);
}

}
