/*
 * arch/arm64/kernel/ftrace.c
 *
 * Copyright (C) 2013 Linaro Limited
 * Author: AKASHI Takahiro <takahiro.akashi@linaro.org>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/ftrace.h>
#include <linux/swab.h>
#include <linux/uaccess.h>

#include <asm/cacheflush.h>
#include <asm/ftrace.h>
#include <asm/insn.h>
#include <asm/sections.h>

#ifdef CONFIG_DYNAMIC_FTRACE

#ifdef CONFIG_DEBUG_RODATA
#include "../mm/mm.h"
#define PAGE_KERNEL_ROX        __pgprot(_PAGE_DEFAULT | PTE_UXN | PTE_DIRTY | PTE_RDONLY)
#endif


int ftrace_arch_code_modify_prepare(void)
{
#ifdef CONFIG_DEBUG_RODATA
	create_mapping_late(__pa(_stext), (unsigned long)_stext,
				__pa(_etext) - __pa(_stext),
				PAGE_KERNEL_EXEC);
#endif
	return 0;
}

int ftrace_arch_code_modify_post_process(void)
{
#ifdef CONFIG_DEBUG_RODATA
	create_mapping_late(__pa(_stext), (unsigned long)_stext,
				__pa(_etext) - __pa(_stext),
				PAGE_KERNEL_ROX);
#endif
	return 0;
}

/*
 * Replace a single instruction, which may be a branch or NOP.
 * If @validate == true, a replaced instruction is checked against 'old'.
 */
static int ftrace_modify_code(unsigned long pc, u32 old, u32 new,
			      bool validate)
{
	u32 replaced;

	/*
	 * Note:
	 * We are paranoid about modifying text, as if a bug were to happen, it
	 * could cause us to read or write to someplace that could cause harm.
	 * Carefully read and modify the code with aarch64_insn_*() which uses
	 * probe_kernel_*(), and make sure what we read is what we expected it
	 * to be before modifying it.
	 */
	if (validate) {
		if (aarch64_insn_read((void *)pc, &replaced))
			return -EFAULT;

		if (replaced != old)
			return -EINVAL;
	}
	if (aarch64_insn_patch_text_nosync((void *)pc, new))
		return -EPERM;

	return 0;
}

/*
 * Replace tracer function in ftrace_caller()
 */
int ftrace_update_ftrace_func(ftrace_func_t func)
{
	unsigned long pc;
	u32 new;

	pc = (unsigned long)&ftrace_call;
	new = aarch64_insn_gen_branch_imm(pc, (unsigned long)func,
					  AARCH64_INSN_BRANCH_LINK);

	return ftrace_modify_code(pc, 0, new, false);
}

/*
 * Turn on the call to ftrace_caller() in instrumented function
 */
int ftrace_make_call(struct dyn_ftrace *rec, unsigned long addr)
{
	unsigned long pc = rec->ip;
	u32 old, new;

	old = aarch64_insn_gen_nop();
	new = aarch64_insn_gen_branch_imm(pc, addr, AARCH64_INSN_BRANCH_LINK);

	return ftrace_modify_code(pc, old, new, true);
}

/*
 * Turn off the call to ftrace_caller() in instrumented function
 */
int ftrace_make_nop(struct module *mod, struct dyn_ftrace *rec,
		    unsigned long addr)
{
	unsigned long pc = rec->ip;
	u32 old, new;

	old = aarch64_insn_gen_branch_imm(pc, addr, AARCH64_INSN_BRANCH_LINK);
	new = aarch64_insn_gen_nop();

	return ftrace_modify_code(pc, old, new, true);
}

void arch_ftrace_update_code(int command)
{
	ftrace_modify_all_code(command);
}

int __init ftrace_dyn_arch_init(void)
{
	return 0;
}
#endif /* CONFIG_DYNAMIC_FTRACE */

#ifdef CONFIG_FUNCTION_GRAPH_TRACER
/*
 * function_graph tracer expects ftrace_return_to_handler() to be called
 * on the way back to parent. For this purpose, this function is called
 * in _mcount() or ftrace_caller() to replace return address (*parent) on
 * the call stack to return_to_handler.
 *
 * Note that @frame_pointer is used only for sanity check later.
 */
void prepare_ftrace_return(unsigned long *parent, unsigned long self_addr,
			   unsigned long frame_pointer)
{
	unsigned long return_hooker = (unsigned long)&return_to_handler;
	unsigned long old;
	struct ftrace_graph_ent trace;
	int err;

	if (unlikely(atomic_read(&current->tracing_graph_pause)))
		return;

	/*
	 * Note:
	 * No protection against faulting at *parent, which may be seen
	 * on other archs. It's unlikely on AArch64.
	 */
	old = *parent;

	trace.func = self_addr;
	trace.depth = current->curr_ret_stack + 1;

	/* Only trace if the calling function expects to */
	if (!ftrace_graph_entry(&trace))
		return;

	err = ftrace_push_return_trace(old, self_addr, &trace.depth,
				       frame_pointer, NULL);
	if (err == -EBUSY)
		return;
	else
		*parent = return_hooker;
}

#ifdef CONFIG_DYNAMIC_FTRACE
/*
 * Turn on/off the call to ftrace_graph_caller() in ftrace_caller()
 * depending on @enable.
 */
static int ftrace_modify_graph_caller(bool enable)
{
	unsigned long pc = (unsigned long)&ftrace_graph_call;
	u32 branch, nop;

	branch = aarch64_insn_gen_branch_imm(pc,
					     (unsigned long)ftrace_graph_caller,
					     AARCH64_INSN_BRANCH_NOLINK);
	nop = aarch64_insn_gen_nop();

	if (enable)
		return ftrace_modify_code(pc, nop, branch, true);
	else
		return ftrace_modify_code(pc, branch, nop, true);
}

int ftrace_enable_ftrace_graph_caller(void)
{
	return ftrace_modify_graph_caller(true);
}

int ftrace_disable_ftrace_graph_caller(void)
{
	return ftrace_modify_graph_caller(false);
}
#endif /* CONFIG_DYNAMIC_FTRACE */
#endif /* CONFIG_FUNCTION_GRAPH_TRACER */
