/*
 * exceptions-amd64.c: exception support for AMD64
 *
 * Authors:
 *   Dietmar Maurer (dietmar@ximian.com)
 *
 * (C) 2001 Ximian, Inc.
 * Copyright 2011 Xamarin, Inc (http://www.xamarin.com)
 * Licensed under the MIT license. See LICENSE file in the project root for full license information.
 */

#include <config.h>

#include <glib.h>
#include <string.h>

#ifdef HAVE_SIGNAL_H
#include <signal.h>
#endif
#ifdef HAVE_UCONTEXT_H
#include <ucontext.h>
#endif

#include <mono/arch/amd64/amd64-codegen.h>
#include <mono/metadata/abi-details.h>
#include <mono/metadata/appdomain.h>
#include <mono/metadata/tabledefs.h>
#include <mono/metadata/threads.h>
#include <mono/metadata/threads-types.h>
#include <mono/metadata/debug-helpers.h>
#include <mono/metadata/exception.h>
#include <mono/metadata/gc-internals.h>
#include <mono/metadata/mono-debug.h>
#include <mono/utils/mono-mmap.h>

#include "mini.h"
#include "mini-amd64.h"
#include "tasklets.h"

#define ALIGN_TO(val,align) (((val) + ((align) - 1)) & ~((align) - 1))

#ifdef TARGET_WIN32
static MonoW32ExceptionHandler fpe_handler;
static MonoW32ExceptionHandler ill_handler;
static MonoW32ExceptionHandler segv_handler;

LPTOP_LEVEL_EXCEPTION_FILTER mono_old_win_toplevel_exception_filter;
void *mono_win_vectored_exception_handle;

#define W32_SEH_HANDLE_EX(_ex) \
	if (_ex##_handler) _ex##_handler(0, ep, ctx)

static LONG CALLBACK seh_unhandled_exception_filter(EXCEPTION_POINTERS* ep)
{
#ifndef MONO_CROSS_COMPILE
	if (mono_old_win_toplevel_exception_filter) {
		return (*mono_old_win_toplevel_exception_filter)(ep);
	}
#endif

	mono_handle_native_sigsegv (SIGSEGV, NULL, NULL);

	return EXCEPTION_CONTINUE_SEARCH;
}

/*
 * Unhandled Exception Filter
 * Top-level per-process exception handler.
 */
LONG CALLBACK seh_vectored_exception_handler(EXCEPTION_POINTERS* ep)
{
	EXCEPTION_RECORD* er;
	CONTEXT* ctx;
	LONG res;
	MonoJitTlsData *jit_tls = mono_native_tls_get_value (mono_jit_tls_id);

	/* If the thread is not managed by the runtime return early */
	if (!jit_tls)
		return EXCEPTION_CONTINUE_SEARCH;

	jit_tls->mono_win_chained_exception_needs_run = FALSE;
	res = EXCEPTION_CONTINUE_EXECUTION;

	er = ep->ExceptionRecord;
	ctx = ep->ContextRecord;

	switch (er->ExceptionCode) {
	case EXCEPTION_ACCESS_VIOLATION:
		W32_SEH_HANDLE_EX(segv);
		break;
	case EXCEPTION_ILLEGAL_INSTRUCTION:
		W32_SEH_HANDLE_EX(ill);
		break;
	case EXCEPTION_INT_DIVIDE_BY_ZERO:
	case EXCEPTION_INT_OVERFLOW:
	case EXCEPTION_FLT_DIVIDE_BY_ZERO:
	case EXCEPTION_FLT_OVERFLOW:
	case EXCEPTION_FLT_UNDERFLOW:
	case EXCEPTION_FLT_INEXACT_RESULT:
		W32_SEH_HANDLE_EX(fpe);
		break;
	default:
		jit_tls->mono_win_chained_exception_needs_run = TRUE;
		break;
	}

	if (jit_tls->mono_win_chained_exception_needs_run) {
		/* Don't copy context back if we chained exception
		* as the handler may have modfied the EXCEPTION_POINTERS
		* directly. We don't pass sigcontext to chained handlers.
		* Return continue search so the UnhandledExceptionFilter
		* can correctly chain the exception.
		*/
		res = EXCEPTION_CONTINUE_SEARCH;
	}

	return res;
}

void win32_seh_init()
{
	mono_old_win_toplevel_exception_filter = SetUnhandledExceptionFilter(seh_unhandled_exception_filter);
	mono_win_vectored_exception_handle = AddVectoredExceptionHandler (1, seh_vectored_exception_handler);
}

void win32_seh_cleanup()
{
	guint32 ret = 0;

	if (mono_old_win_toplevel_exception_filter) SetUnhandledExceptionFilter(mono_old_win_toplevel_exception_filter);

	ret = RemoveVectoredExceptionHandler (mono_win_vectored_exception_handle);
	g_assert (ret);
}

void win32_seh_set_handler(int type, MonoW32ExceptionHandler handler)
{
	switch (type) {
	case SIGFPE:
		fpe_handler = handler;
		break;
	case SIGILL:
		ill_handler = handler;
		break;
	case SIGSEGV:
		segv_handler = handler;
		break;
	default:
		break;
	}
}

#endif /* TARGET_WIN32 */

/*
 * mono_arch_get_restore_context:
 *
 * Returns a pointer to a method which restores a previously saved sigcontext.
 */
gpointer
mono_arch_get_restore_context (MonoTrampInfo **info, gboolean aot)
{
	guint8 *start = NULL;
	guint8 *code;
	MonoJumpInfo *ji = NULL;
	GSList *unwind_ops = NULL;
	int i, gregs_offset;

	/* restore_contect (MonoContext *ctx) */

	start = code = (guint8 *)mono_global_codeman_reserve (256);

	amd64_mov_reg_reg (code, AMD64_R11, AMD64_ARG_REG1, 8);

	/* Restore all registers except %rip and %r11 */
	gregs_offset = MONO_STRUCT_OFFSET (MonoContext, gregs);
	for (i = 0; i < AMD64_NREG; ++i) {
		if (i != AMD64_RIP && i != AMD64_RSP && i != AMD64_R8 && i != AMD64_R9 && i != AMD64_R10 && i != AMD64_R11)
			amd64_mov_reg_membase (code, i, AMD64_R11, gregs_offset + (i * 8), 8);
	}

	/*
	 * The context resides on the stack, in the stack frame of the
	 * caller of this function.  The stack pointer that we need to
	 * restore is potentially many stack frames higher up, so the
	 * distance between them can easily be more than the red zone
	 * size.  Hence the stack pointer can be restored only after
	 * we have finished loading everything from the context.
	 */
	amd64_mov_reg_membase (code, AMD64_R8, AMD64_R11,  gregs_offset + (AMD64_RSP * 8), 8);
	amd64_mov_reg_membase (code, AMD64_R11, AMD64_R11,  gregs_offset + (AMD64_RIP * 8), 8);
	amd64_mov_reg_reg (code, AMD64_RSP, AMD64_R8, 8);

	/* jump to the saved IP */
	amd64_jump_reg (code, AMD64_R11);

	mono_arch_flush_icache (start, code - start);
	mono_profiler_code_buffer_new (start, code - start, MONO_PROFILER_CODE_BUFFER_EXCEPTION_HANDLING, NULL);

	if (info)
		*info = mono_tramp_info_create ("restore_context", start, code - start, ji, unwind_ops);

	return start;
}

/*
 * mono_arch_get_call_filter:
 *
 * Returns a pointer to a method which calls an exception filter. We
 * also use this function to call finally handlers (we pass NULL as 
 * @exc object in this case).
 */
gpointer
mono_arch_get_call_filter (MonoTrampInfo **info, gboolean aot)
{
	guint8 *start;
	int i, gregs_offset;
	guint8 *code;
	guint32 pos;
	MonoJumpInfo *ji = NULL;
	GSList *unwind_ops = NULL;
	const guint kMaxCodeSize = 128;

	start = code = (guint8 *)mono_global_codeman_reserve (kMaxCodeSize);

	/* call_filter (MonoContext *ctx, unsigned long eip) */
	code = start;

	/* Alloc new frame */
	amd64_push_reg (code, AMD64_RBP);
	amd64_mov_reg_reg (code, AMD64_RBP, AMD64_RSP, 8);

	/* Save callee saved regs */
	pos = 0;
	for (i = 0; i < AMD64_NREG; ++i)
		if (AMD64_IS_CALLEE_SAVED_REG (i)) {
			amd64_push_reg (code, i);
			pos += 8;
		}

	/* Save EBP */
	pos += 8;
	amd64_push_reg (code, AMD64_RBP);

	/* Make stack misaligned, the call will make it aligned again */
	if (! (pos & 8))
		amd64_alu_reg_imm (code, X86_SUB, AMD64_RSP, 8);

	gregs_offset = MONO_STRUCT_OFFSET (MonoContext, gregs);

	/* set new EBP */
	amd64_mov_reg_membase (code, AMD64_RBP, AMD64_ARG_REG1, gregs_offset + (AMD64_RBP * 8), 8);
	/* load callee saved regs */
	for (i = 0; i < AMD64_NREG; ++i) {
		if (AMD64_IS_CALLEE_SAVED_REG (i) && i != AMD64_RBP)
			amd64_mov_reg_membase (code, i, AMD64_ARG_REG1, gregs_offset + (i * 8), 8);
	}
	/* load exc register */
	amd64_mov_reg_membase (code, AMD64_RAX, AMD64_ARG_REG1,  gregs_offset + (AMD64_RAX * 8), 8);

	/* call the handler */
	amd64_call_reg (code, AMD64_ARG_REG2);

	if (! (pos & 8))
		amd64_alu_reg_imm (code, X86_ADD, AMD64_RSP, 8);

	/* restore RBP */
	amd64_pop_reg (code, AMD64_RBP);

	/* Restore callee saved regs */
	for (i = AMD64_NREG; i >= 0; --i)
		if (AMD64_IS_CALLEE_SAVED_REG (i))
			amd64_pop_reg (code, i);

	amd64_leave (code);
	amd64_ret (code);

	g_assert ((code - start) < kMaxCodeSize);

	mono_arch_flush_icache (start, code - start);
	mono_profiler_code_buffer_new (start, code - start, MONO_PROFILER_CODE_BUFFER_EXCEPTION_HANDLING, NULL);

	if (info)
		*info = mono_tramp_info_create ("call_filter", start, code - start, ji, unwind_ops);

	return start;
}

/* 
 * The first few arguments are dummy, to force the other arguments to be passed on
 * the stack, this avoids overwriting the argument registers in the throw trampoline.
 */
void
mono_amd64_throw_exception (guint64 dummy1, guint64 dummy2, guint64 dummy3, guint64 dummy4,
							guint64 dummy5, guint64 dummy6,
							MonoContext *mctx, MonoObject *exc, gboolean rethrow)
{
	MonoError error;
	MonoContext ctx;

	/* mctx is on the caller's stack */
	memcpy (&ctx, mctx, sizeof (MonoContext));

	if (mono_object_isinst_checked (exc, mono_defaults.exception_class, &error)) {
		MonoException *mono_ex = (MonoException*)exc;
		if (!rethrow) {
			mono_ex->stack_trace = NULL;
			mono_ex->trace_ips = NULL;
		}
	}
	mono_error_assert_ok (&error);

	/* adjust eip so that it point into the call instruction */
	ctx.gregs [AMD64_RIP] --;

	mono_handle_exception (&ctx, exc);
	mono_restore_context (&ctx);
	g_assert_not_reached ();
}

void
mono_amd64_throw_corlib_exception (guint64 dummy1, guint64 dummy2, guint64 dummy3, guint64 dummy4,
								   guint64 dummy5, guint64 dummy6,
								   MonoContext *mctx, guint32 ex_token_index, gint64 pc_offset)
{
	guint32 ex_token = MONO_TOKEN_TYPE_DEF | ex_token_index;
	MonoException *ex;

	ex = mono_exception_from_token (mono_defaults.exception_class->image, ex_token);

	mctx->gregs [AMD64_RIP] -= pc_offset;

	/* Negate the ip adjustment done in mono_amd64_throw_exception () */
	mctx->gregs [AMD64_RIP] += 1;

	mono_amd64_throw_exception (dummy1, dummy2, dummy3, dummy4, dummy5, dummy6, mctx, (MonoObject*)ex, FALSE);
}

void
mono_amd64_resume_unwind (guint64 dummy1, guint64 dummy2, guint64 dummy3, guint64 dummy4,
						  guint64 dummy5, guint64 dummy6,
						  MonoContext *mctx, guint32 dummy7, gint64 dummy8)
{
	/* Only the register parameters are valid */
	MonoContext ctx;

	/* mctx is on the caller's stack */
	memcpy (&ctx, mctx, sizeof (MonoContext));

	mono_resume_unwind (&ctx);
}

/*
 * get_throw_trampoline:
 *
 *  Generate a call to mono_amd64_throw_exception/
 * mono_amd64_throw_corlib_exception.
 */
static gpointer
get_throw_trampoline (MonoTrampInfo **info, gboolean rethrow, gboolean corlib, gboolean llvm_abs, gboolean resume_unwind, const char *tramp_name, gboolean aot)
{
	guint8* start;
	guint8 *code;
	MonoJumpInfo *ji = NULL;
	GSList *unwind_ops = NULL;
	int i, stack_size, arg_offsets [16], ctx_offset, regs_offset, dummy_stack_space;
	const guint kMaxCodeSize = 256;

#ifdef TARGET_WIN32
	dummy_stack_space = 6 * sizeof(mgreg_t);	/* Windows expects stack space allocated for all 6 dummy args. */
#else
	dummy_stack_space = 0;
#endif

	start = code = (guint8 *)mono_global_codeman_reserve (kMaxCodeSize);

	/* The stack is unaligned on entry */
	stack_size = ALIGN_TO (sizeof (MonoContext) + 64 + dummy_stack_space, MONO_ARCH_FRAME_ALIGNMENT) + 8;

	code = start;

	if (info)
		unwind_ops = mono_arch_get_cie_program ();

	/* Alloc frame */
	amd64_alu_reg_imm (code, X86_SUB, AMD64_RSP, stack_size);
	if (info)
		mono_add_unwind_op_def_cfa_offset (unwind_ops, code, start, stack_size + 8);

	/*
	 * To hide linux/windows calling convention differences, we pass all arguments on
	 * the stack by passing 6 dummy values in registers.
	 */

	arg_offsets [0] = dummy_stack_space + 0;
	arg_offsets [1] = dummy_stack_space + sizeof(mgreg_t);
	arg_offsets [2] = dummy_stack_space + sizeof(mgreg_t) * 2;
	ctx_offset = dummy_stack_space + sizeof(mgreg_t) * 4;
	regs_offset = ctx_offset + MONO_STRUCT_OFFSET (MonoContext, gregs);

	/* Save registers */
	for (i = 0; i < AMD64_NREG; ++i)
		if (i != AMD64_RSP)
			amd64_mov_membase_reg (code, AMD64_RSP, regs_offset + (i * sizeof(mgreg_t)), i, sizeof(mgreg_t));
	/* Save RSP */
	amd64_lea_membase (code, AMD64_RAX, AMD64_RSP, stack_size + sizeof(mgreg_t));
	amd64_mov_membase_reg (code, AMD64_RSP, regs_offset + (AMD64_RSP * sizeof(mgreg_t)), X86_EAX, sizeof(mgreg_t));
	/* Save IP */
	amd64_mov_reg_membase (code, AMD64_RAX, AMD64_RSP, stack_size, sizeof(mgreg_t));
	amd64_mov_membase_reg (code, AMD64_RSP, regs_offset + (AMD64_RIP * sizeof(mgreg_t)), AMD64_RAX, sizeof(mgreg_t));
	/* Set arg1 == ctx */
	amd64_lea_membase (code, AMD64_RAX, AMD64_RSP, ctx_offset);
	amd64_mov_membase_reg (code, AMD64_RSP, arg_offsets [0], AMD64_RAX, sizeof(mgreg_t));
	/* Set arg2 == exc/ex_token_index */
	if (resume_unwind)
		amd64_mov_membase_imm (code, AMD64_RSP, arg_offsets [1], 0, sizeof(mgreg_t));
	else
		amd64_mov_membase_reg (code, AMD64_RSP, arg_offsets [1], AMD64_ARG_REG1, sizeof(mgreg_t));
	/* Set arg3 == rethrow/pc offset */
	if (resume_unwind) {
		amd64_mov_membase_imm (code, AMD64_RSP, arg_offsets [2], 0, sizeof(mgreg_t));
	} else if (corlib) {
		if (llvm_abs)
			/*
			 * The caller doesn't pass in a pc/pc offset, instead we simply use the
			 * caller ip. Negate the pc adjustment done in mono_amd64_throw_corlib_exception ().
			 */
			amd64_mov_membase_imm (code, AMD64_RSP, arg_offsets [2], 1, sizeof(mgreg_t));
		else
			amd64_mov_membase_reg (code, AMD64_RSP, arg_offsets [2], AMD64_ARG_REG2, sizeof(mgreg_t));
	} else {
		amd64_mov_membase_imm (code, AMD64_RSP, arg_offsets [2], rethrow, sizeof(mgreg_t));
	}

	if (aot) {
		const char *icall_name;

		if (resume_unwind)
			icall_name = "mono_amd64_resume_unwind";
		else if (corlib)
			icall_name = "mono_amd64_throw_corlib_exception";
		else
			icall_name = "mono_amd64_throw_exception";
		ji = mono_patch_info_list_prepend (ji, code - start, MONO_PATCH_INFO_JIT_ICALL_ADDR, icall_name);
		amd64_mov_reg_membase (code, AMD64_R11, AMD64_RIP, 0, 8);
	} else {
		amd64_mov_reg_imm (code, AMD64_R11, resume_unwind ? ((gpointer)mono_amd64_resume_unwind) : (corlib ? (gpointer)mono_amd64_throw_corlib_exception : (gpointer)mono_amd64_throw_exception));
	}
	amd64_call_reg (code, AMD64_R11);
	amd64_breakpoint (code);

	mono_arch_flush_icache (start, code - start);

	g_assert ((code - start) < kMaxCodeSize);

	mono_profiler_code_buffer_new (start, code - start, MONO_PROFILER_CODE_BUFFER_EXCEPTION_HANDLING, NULL);

	if (info)
		*info = mono_tramp_info_create (tramp_name, start, code - start, ji, unwind_ops);

	return start;
}

/**
 * mono_arch_get_throw_exception:
 *
 * Returns a function pointer which can be used to raise 
 * exceptions. The returned function has the following 
 * signature: void (*func) (MonoException *exc); 
 *
 */
gpointer
mono_arch_get_throw_exception (MonoTrampInfo **info, gboolean aot)
{
	return get_throw_trampoline (info, FALSE, FALSE, FALSE, FALSE, "throw_exception", aot);
}

gpointer 
mono_arch_get_rethrow_exception (MonoTrampInfo **info, gboolean aot)
{
	return get_throw_trampoline (info, TRUE, FALSE, FALSE, FALSE, "rethrow_exception", aot);
}

/**
 * mono_arch_get_throw_corlib_exception:
 *
 * Returns a function pointer which can be used to raise 
 * corlib exceptions. The returned function has the following 
 * signature: void (*func) (guint32 ex_token, guint32 offset); 
 * Here, offset is the offset which needs to be substracted from the caller IP 
 * to get the IP of the throw. Passing the offset has the advantage that it 
 * needs no relocations in the caller.
 */
gpointer 
mono_arch_get_throw_corlib_exception (MonoTrampInfo **info, gboolean aot)
{
	return get_throw_trampoline (info, FALSE, TRUE, FALSE, FALSE, "throw_corlib_exception", aot);
}

/*
 * mono_arch_unwind_frame:
 *
 * This function is used to gather information from @ctx, and store it in @frame_info.
 * It unwinds one stack frame, and stores the resulting context into @new_ctx. @lmf
 * is modified if needed.
 * Returns TRUE on success, FALSE otherwise.
 */
gboolean
mono_arch_unwind_frame (MonoDomain *domain, MonoJitTlsData *jit_tls, 
							 MonoJitInfo *ji, MonoContext *ctx, 
							 MonoContext *new_ctx, MonoLMF **lmf,
							 mgreg_t **save_locations,
							 StackFrameInfo *frame)
{
	gpointer ip = MONO_CONTEXT_GET_IP (ctx);
	int i;

	memset (frame, 0, sizeof (StackFrameInfo));
	frame->ji = ji;

	*new_ctx = *ctx;

	if (ji != NULL) {
		mgreg_t regs [MONO_MAX_IREGS + 1];
		guint8 *cfa;
		guint32 unwind_info_len;
		guint8 *unwind_info;
		guint8 *epilog = NULL;

		if (ji->is_trampoline)
			frame->type = FRAME_TYPE_TRAMPOLINE;
		else
			frame->type = FRAME_TYPE_MANAGED;

		unwind_info = mono_jinfo_get_unwind_info (ji, &unwind_info_len);

		frame->unwind_info = unwind_info;
		frame->unwind_info_len = unwind_info_len;

		/*
		printf ("%s %p %p\n", ji->d.method->name, ji->code_start, ip);
		mono_print_unwind_info (unwind_info, unwind_info_len);
		*/
		/* LLVM compiled code doesn't have this info */
		if (ji->has_arch_eh_info)
			epilog = (guint8*)ji->code_start + ji->code_size - mono_jinfo_get_epilog_size (ji);
 
		for (i = 0; i < AMD64_NREG; ++i)
			regs [i] = new_ctx->gregs [i];

		mono_unwind_frame (unwind_info, unwind_info_len, (guint8 *)ji->code_start,
						   (guint8*)ji->code_start + ji->code_size,
						   (guint8 *)ip, epilog ? &epilog : NULL, regs, MONO_MAX_IREGS + 1,
						   save_locations, MONO_MAX_IREGS, &cfa);

		for (i = 0; i < AMD64_NREG; ++i)
			new_ctx->gregs [i] = regs [i];
 
		/* The CFA becomes the new SP value */
		new_ctx->gregs [AMD64_RSP] = (mgreg_t)cfa;

		/* Adjust IP */
		new_ctx->gregs [AMD64_RIP] --;

		return TRUE;
	} else if (*lmf) {
		guint64 rip;

		if (((guint64)(*lmf)->previous_lmf) & 2) {
			/* 
			 * This LMF entry is created by the soft debug code to mark transitions to
			 * managed code done during invokes.
			 */
			MonoLMFExt *ext = (MonoLMFExt*)(*lmf);

			g_assert (ext->debugger_invoke);

			memcpy (new_ctx, &ext->ctx, sizeof (MonoContext));

			*lmf = (MonoLMF *)(((guint64)(*lmf)->previous_lmf) & ~7);

			frame->type = FRAME_TYPE_DEBUGGER_INVOKE;

			return TRUE;
		}

		if (((guint64)(*lmf)->previous_lmf) & 4) {
			MonoLMFTramp *ext = (MonoLMFTramp*)(*lmf);

			rip = (guint64)MONO_CONTEXT_GET_IP (ext->ctx);
		} else if (((guint64)(*lmf)->previous_lmf) & 1) {
			/* This LMF has the rip field set */
			rip = (*lmf)->rip;
		} else if ((*lmf)->rsp == 0) {
			/* Top LMF entry */
			return FALSE;
		} else {
			/* 
			 * The rsp field is set just before the call which transitioned to native 
			 * code. Obtain the rip from the stack.
			 */
			rip = *(guint64*)((*lmf)->rsp - sizeof(mgreg_t));
		}

		ji = mini_jit_info_table_find (domain, (char *)rip, NULL);
		/*
		 * FIXME: ji == NULL can happen when a managed-to-native wrapper is interrupted
		 * in the soft debugger suspend code, since (*lmf)->rsp no longer points to the
		 * return address.
		 */
		//g_assert (ji);
		if (!ji)
			return FALSE;

		frame->ji = ji;
		frame->type = FRAME_TYPE_MANAGED_TO_NATIVE;

		if (((guint64)(*lmf)->previous_lmf) & 4) {
			MonoLMFTramp *ext = (MonoLMFTramp*)(*lmf);

			/* Trampoline frame */
			for (i = 0; i < AMD64_NREG; ++i)
				new_ctx->gregs [i] = ext->ctx->gregs [i];
			/* Adjust IP */
			new_ctx->gregs [AMD64_RIP] --;
		} else {
			/*
			 * The registers saved in the LMF will be restored using the normal unwind info,
			 * when the wrapper frame is processed.
			 */
			/* Adjust IP */
			rip --;
			new_ctx->gregs [AMD64_RIP] = rip;
			new_ctx->gregs [AMD64_RSP] = (*lmf)->rsp;
			new_ctx->gregs [AMD64_RBP] = (*lmf)->rbp;
			for (i = 0; i < AMD64_NREG; ++i) {
				if (AMD64_IS_CALLEE_SAVED_REG (i) && i != AMD64_RBP)
					new_ctx->gregs [i] = 0;
			}
		}

		*lmf = (MonoLMF *)(((guint64)(*lmf)->previous_lmf) & ~7);

		return TRUE;
	}

	return FALSE;
}

/*
 * handle_exception:
 *
 *   Called by resuming from a signal handler.
 */
static void
handle_signal_exception (gpointer obj)
{
	MonoJitTlsData *jit_tls = (MonoJitTlsData *)mono_native_tls_get_value (mono_jit_tls_id);
	MonoContext ctx;

	memcpy (&ctx, &jit_tls->ex_ctx, sizeof (MonoContext));

	mono_handle_exception (&ctx, (MonoObject *)obj);

	mono_restore_context (&ctx);
}

void
mono_arch_setup_async_callback (MonoContext *ctx, void (*async_cb)(void *fun), gpointer user_data)
{
	guint64 sp = ctx->gregs [AMD64_RSP];

	ctx->gregs [AMD64_RDI] = (guint64)user_data;

	/* Allocate a stack frame below the red zone */
	sp -= 128;
	/* The stack should be unaligned */
	if ((sp % 16) == 0)
		sp -= 8;
#ifdef __linux__
	/* Preserve the call chain to prevent crashes in the libgcc unwinder (#15969) */
	*(guint64*)sp = ctx->gregs [AMD64_RIP];
#endif
	ctx->gregs [AMD64_RSP] = sp;
	ctx->gregs [AMD64_RIP] = (guint64)async_cb;
}

/**
 * mono_arch_handle_exception:
 *
 * @ctx: saved processor state
 * @obj: the exception object
 */
gboolean
mono_arch_handle_exception (void *sigctx, gpointer obj)
{
#if defined(MONO_ARCH_USE_SIGACTION)
	MonoContext mctx;

	/*
	 * Handling the exception in the signal handler is problematic, since the original
	 * signal is disabled, and we could run arbitrary code though the debugger. So
	 * resume into the normal stack and do most work there if possible.
	 */
	MonoJitTlsData *jit_tls = (MonoJitTlsData *)mono_native_tls_get_value (mono_jit_tls_id);

	/* Pass the ctx parameter in TLS */
	mono_sigctx_to_monoctx (sigctx, &jit_tls->ex_ctx);

	mctx = jit_tls->ex_ctx;
	mono_arch_setup_async_callback (&mctx, handle_signal_exception, obj);
	mono_monoctx_to_sigctx (&mctx, sigctx);

	return TRUE;
#else
	MonoContext mctx;

	mono_sigctx_to_monoctx (sigctx, &mctx);

	mono_handle_exception (&mctx, obj);

	mono_monoctx_to_sigctx (&mctx, sigctx);

	return TRUE;
#endif
}

gpointer
mono_arch_ip_from_context (void *sigctx)
{
#if defined(MONO_ARCH_USE_SIGACTION)
	ucontext_t *ctx = (ucontext_t*)sigctx;

	return (gpointer)UCONTEXT_REG_RIP (ctx);
#elif defined(HOST_WIN32)
	return ((CONTEXT*)sigctx)->Rip;
#else
	MonoContext *ctx = sigctx;
	return (gpointer)ctx->gregs [AMD64_RIP];
#endif	
}

static void
restore_soft_guard_pages (void)
{
	MonoJitTlsData *jit_tls = (MonoJitTlsData *)mono_native_tls_get_value (mono_jit_tls_id);
	if (jit_tls->stack_ovf_guard_base)
		mono_mprotect (jit_tls->stack_ovf_guard_base, jit_tls->stack_ovf_guard_size, MONO_MMAP_NONE);
}

/* 
 * this function modifies mctx so that when it is restored, it
 * won't execcute starting at mctx.eip, but in a function that
 * will restore the protection on the soft-guard pages and return back to
 * continue at mctx.eip.
 */
static void
prepare_for_guard_pages (MonoContext *mctx)
{
	gpointer *sp;
	sp = (gpointer *)(mctx->gregs [AMD64_RSP]);
	sp -= 1;
	/* the return addr */
	sp [0] = (gpointer)(mctx->gregs [AMD64_RIP]);
	mctx->gregs [AMD64_RIP] = (guint64)restore_soft_guard_pages;
	mctx->gregs [AMD64_RSP] = (guint64)sp;
}

static void
altstack_handle_and_restore (MonoContext *ctx, MonoObject *obj, gboolean stack_ovf)
{
	MonoContext mctx;

	mctx = *ctx;

	mono_handle_exception (&mctx, obj);
	if (stack_ovf)
		prepare_for_guard_pages (&mctx);
	mono_restore_context (&mctx);
}

void
mono_arch_handle_altstack_exception (void *sigctx, MONO_SIG_HANDLER_INFO_TYPE *siginfo, gpointer fault_addr, gboolean stack_ovf)
{
#if defined(MONO_ARCH_USE_SIGACTION)
	MonoException *exc = NULL;
	MonoJitInfo *ji = mini_jit_info_table_find (mono_domain_get (), (char *)UCONTEXT_REG_RIP (sigctx), NULL);
	gpointer *sp;
	int frame_size;
	MonoContext *copied_ctx;

	if (stack_ovf)
		exc = mono_domain_get ()->stack_overflow_ex;
	if (!ji)
		mono_handle_native_sigsegv (SIGSEGV, sigctx, siginfo);

	/* setup a call frame on the real stack so that control is returned there
	 * and exception handling can continue.
	 * The frame looks like:
	 *   ucontext struct
	 *   ...
	 *   return ip
	 * 128 is the size of the red zone
	 */
	frame_size = sizeof (MonoContext) + sizeof (gpointer) * 4 + 128;
	frame_size += 15;
	frame_size &= ~15;
	sp = (gpointer *)(UCONTEXT_REG_RSP (sigctx) & ~15);
	sp = (gpointer *)((char*)sp - frame_size);
	copied_ctx = (MonoContext*)(sp + 4);
	/* the arguments must be aligned */
	sp [-1] = (gpointer)UCONTEXT_REG_RIP (sigctx);
	mono_sigctx_to_monoctx (sigctx, copied_ctx);
	/* at the return form the signal handler execution starts in altstack_handle_and_restore() */
	UCONTEXT_REG_RIP (sigctx) = (unsigned long)altstack_handle_and_restore;
	UCONTEXT_REG_RSP (sigctx) = (unsigned long)(sp - 1);
	UCONTEXT_REG_RDI (sigctx) = (unsigned long)(copied_ctx);
	UCONTEXT_REG_RSI (sigctx) = (guint64)exc;
	UCONTEXT_REG_RDX (sigctx) = stack_ovf;
#endif
}

guint64
mono_amd64_get_original_ip (void)
{
	MonoLMF *lmf = mono_get_lmf ();

	g_assert (lmf);

	/* Reset the change to previous_lmf */
	lmf->previous_lmf = (gpointer)((guint64)lmf->previous_lmf & ~1);

	return lmf->rip;
}

GSList*
mono_amd64_get_exception_trampolines (gboolean aot)
{
	MonoTrampInfo *info;
	GSList *tramps = NULL;

	/* LLVM needs different throw trampolines */
	get_throw_trampoline (&info, FALSE, TRUE, FALSE, FALSE, "llvm_throw_corlib_exception_trampoline", aot);
	tramps = g_slist_prepend (tramps, info);

	get_throw_trampoline (&info, FALSE, TRUE, TRUE, FALSE, "llvm_throw_corlib_exception_abs_trampoline", aot);
	tramps = g_slist_prepend (tramps, info);

	get_throw_trampoline (&info, FALSE, TRUE, TRUE, TRUE, "llvm_resume_unwind_trampoline", aot);
	tramps = g_slist_prepend (tramps, info);

	return tramps;
}

void
mono_arch_exceptions_init (void)
{
	GSList *tramps, *l;
	gpointer tramp;

	if (mono_aot_only) {
		tramp = mono_aot_get_trampoline ("llvm_throw_corlib_exception_trampoline");
		mono_register_jit_icall (tramp, "llvm_throw_corlib_exception_trampoline", NULL, TRUE);
		tramp = mono_aot_get_trampoline ("llvm_throw_corlib_exception_abs_trampoline");
		mono_register_jit_icall (tramp, "llvm_throw_corlib_exception_abs_trampoline", NULL, TRUE);
		tramp = mono_aot_get_trampoline ("llvm_resume_unwind_trampoline");
		mono_register_jit_icall (tramp, "llvm_resume_unwind_trampoline", NULL, TRUE);
	} else {
		/* Call this to avoid initialization races */
		tramps = mono_amd64_get_exception_trampolines (FALSE);
		for (l = tramps; l; l = l->next) {
			MonoTrampInfo *info = (MonoTrampInfo *)l->data;

			mono_register_jit_icall (info->code, g_strdup (info->name), NULL, TRUE);
			mono_tramp_info_register (info, NULL);
		}
		g_slist_free (tramps);
	}
}

#ifdef TARGET_WIN32

/*
 * The mono_arch_unwindinfo* methods are used to build and add
 * function table info for each emitted method from mono.  On Winx64
 * the seh handler will not be called if the mono methods are not
 * added to the function table.  
 *
 * We should not need to add non-volatile register info to the 
 * table since mono stores that info elsewhere. (Except for the register 
 * used for the fp.)
 */

#define MONO_MAX_UNWIND_CODES 22

typedef union _UNWIND_CODE {
    struct {
        guchar CodeOffset;
        guchar UnwindOp : 4;
        guchar OpInfo   : 4;
    };
    gushort FrameOffset;
} UNWIND_CODE, *PUNWIND_CODE;

typedef struct _UNWIND_INFO {
	guchar Version       : 3;
	guchar Flags         : 5;
	guchar SizeOfProlog;
	guchar CountOfCodes;
	guchar FrameRegister : 4;
	guchar FrameOffset   : 4;
	/* custom size for mono allowing for mono allowing for*/
	/*UWOP_PUSH_NONVOL ebp offset = 21*/
	/*UWOP_ALLOC_LARGE : requires 2 or 3 offset = 20*/
	/*UWOP_SET_FPREG : requires 2 offset = 17*/
	/*UWOP_PUSH_NONVOL offset = 15-0*/
	UNWIND_CODE UnwindCode[MONO_MAX_UNWIND_CODES]; 

/*  	UNWIND_CODE MoreUnwindCode[((CountOfCodes + 1) & ~1) - 1];
 *   	union {
 *   	    OPTIONAL ULONG ExceptionHandler;
 *   	    OPTIONAL ULONG FunctionEntry;
 *   	};
 *   	OPTIONAL ULONG ExceptionData[]; */
} UNWIND_INFO, *PUNWIND_INFO;

typedef struct
{
	RUNTIME_FUNCTION runtimeFunction;
	UNWIND_INFO unwindInfo;
} MonoUnwindInfo, *PMonoUnwindInfo;

static void
mono_arch_unwindinfo_create (gpointer* monoui)
{
	PMonoUnwindInfo newunwindinfo;
	*monoui = newunwindinfo = g_new0 (MonoUnwindInfo, 1);
	newunwindinfo->unwindInfo.Version = 1;
}

void
mono_arch_unwindinfo_add_push_nonvol (gpointer* monoui, gpointer codebegin, gpointer nextip, guchar reg )
{
	PMonoUnwindInfo unwindinfo;
	PUNWIND_CODE unwindcode;
	guchar codeindex;
	if (!*monoui)
		mono_arch_unwindinfo_create (monoui);
	
	unwindinfo = (MonoUnwindInfo*)*monoui;

	if (unwindinfo->unwindInfo.CountOfCodes >= MONO_MAX_UNWIND_CODES)
		g_error ("Larger allocation needed for the unwind information.");

	codeindex = MONO_MAX_UNWIND_CODES - (++unwindinfo->unwindInfo.CountOfCodes);
	unwindcode = &unwindinfo->unwindInfo.UnwindCode[codeindex];
	unwindcode->UnwindOp = 0; /*UWOP_PUSH_NONVOL*/
	unwindcode->CodeOffset = (((guchar*)nextip)-((guchar*)codebegin));
	unwindcode->OpInfo = reg;

	if (unwindinfo->unwindInfo.SizeOfProlog >= unwindcode->CodeOffset)
		g_error ("Adding unwind info in wrong order.");
	
	unwindinfo->unwindInfo.SizeOfProlog = unwindcode->CodeOffset;
}

void
mono_arch_unwindinfo_add_set_fpreg (gpointer* monoui, gpointer codebegin, gpointer nextip, guchar reg )
{
	PMonoUnwindInfo unwindinfo;
	PUNWIND_CODE unwindcode;
	guchar codeindex;
	if (!*monoui)
		mono_arch_unwindinfo_create (monoui);
	
	unwindinfo = (MonoUnwindInfo*)*monoui;

	if (unwindinfo->unwindInfo.CountOfCodes + 1 >= MONO_MAX_UNWIND_CODES)
		g_error ("Larger allocation needed for the unwind information.");

	codeindex = MONO_MAX_UNWIND_CODES - (unwindinfo->unwindInfo.CountOfCodes += 2);
	unwindcode = &unwindinfo->unwindInfo.UnwindCode[codeindex];
	unwindcode->FrameOffset = 0; /*Assuming no frame pointer offset for mono*/
	unwindcode++;
	unwindcode->UnwindOp = 3; /*UWOP_SET_FPREG*/
	unwindcode->CodeOffset = (((guchar*)nextip)-((guchar*)codebegin));
	unwindcode->OpInfo = reg;
	
	unwindinfo->unwindInfo.FrameRegister = reg;

	if (unwindinfo->unwindInfo.SizeOfProlog >= unwindcode->CodeOffset)
		g_error ("Adding unwind info in wrong order.");
	
	unwindinfo->unwindInfo.SizeOfProlog = unwindcode->CodeOffset;
}

void
mono_arch_unwindinfo_add_alloc_stack (gpointer* monoui, gpointer codebegin, gpointer nextip, guint size )
{
	PMonoUnwindInfo unwindinfo;
	PUNWIND_CODE unwindcode;
	guchar codeindex;
	guchar codesneeded;
	if (!*monoui)
		mono_arch_unwindinfo_create (monoui);
	
	unwindinfo = (MonoUnwindInfo*)*monoui;

	if (size < 0x8)
		g_error ("Stack allocation must be equal to or greater than 0x8.");
	
	if (size <= 0x80)
		codesneeded = 1;
	else if (size <= 0x7FFF8)
		codesneeded = 2;
	else
		codesneeded = 3;
	
	if (unwindinfo->unwindInfo.CountOfCodes + codesneeded > MONO_MAX_UNWIND_CODES)
		g_error ("Larger allocation needed for the unwind information.");

	codeindex = MONO_MAX_UNWIND_CODES - (unwindinfo->unwindInfo.CountOfCodes += codesneeded);
	unwindcode = &unwindinfo->unwindInfo.UnwindCode[codeindex];

	if (codesneeded == 1) {
		/*The size of the allocation is 
		  (the number in the OpInfo member) times 8 plus 8*/
		unwindcode->OpInfo = (size - 8)/8;
		unwindcode->UnwindOp = 2; /*UWOP_ALLOC_SMALL*/
	}
	else {
		if (codesneeded == 3) {
			/*the unscaled size of the allocation is recorded
			  in the next two slots in little-endian format*/
			*((unsigned int*)(&unwindcode->FrameOffset)) = size;
			unwindcode += 2;
			unwindcode->OpInfo = 1;
		}
		else {
			/*the size of the allocation divided by 8
			  is recorded in the next slot*/
			unwindcode->FrameOffset = size/8; 
			unwindcode++;	
			unwindcode->OpInfo = 0;
			
		}
		unwindcode->UnwindOp = 1; /*UWOP_ALLOC_LARGE*/
	}

	unwindcode->CodeOffset = (((guchar*)nextip)-((guchar*)codebegin));

	if (unwindinfo->unwindInfo.SizeOfProlog >= unwindcode->CodeOffset)
		g_error ("Adding unwind info in wrong order.");
	
	unwindinfo->unwindInfo.SizeOfProlog = unwindcode->CodeOffset;
}

guint
mono_arch_unwindinfo_get_size (gpointer monoui)
{
	PMonoUnwindInfo unwindinfo;
	if (!monoui)
		return 0;
	
	unwindinfo = (MonoUnwindInfo*)monoui;
	return (8 + sizeof (MonoUnwindInfo)) - 
		(sizeof (UNWIND_CODE) * (MONO_MAX_UNWIND_CODES - unwindinfo->unwindInfo.CountOfCodes));
}

static PRUNTIME_FUNCTION
MONO_GET_RUNTIME_FUNCTION_CALLBACK ( DWORD64 ControlPc, IN PVOID Context )
{
	MonoJitInfo *ji;
	guint64 pos;
	PMonoUnwindInfo targetinfo;
	MonoDomain *domain = mono_domain_get ();

	ji = mini_jit_info_table_find (domain, (char*)ControlPc, NULL);
	if (!ji)
		return 0;

	pos = (guint64)(((char*)ji->code_start) + ji->code_size);
	
	targetinfo = (PMonoUnwindInfo)ALIGN_TO (pos, 8);

	targetinfo->runtimeFunction.UnwindData = ((DWORD64)&targetinfo->unwindInfo) - ((DWORD64)Context);

	return &targetinfo->runtimeFunction;
}

void
mono_arch_unwindinfo_install_unwind_info (gpointer* monoui, gpointer code, guint code_size)
{
	PMonoUnwindInfo unwindinfo, targetinfo;
	guchar codecount;
	guint64 targetlocation;
	if (!*monoui)
		return;

	unwindinfo = (MonoUnwindInfo*)*monoui;
	targetlocation = (guint64)&(((guchar*)code)[code_size]);
	targetinfo = (PMonoUnwindInfo) ALIGN_TO(targetlocation, 8);

	unwindinfo->runtimeFunction.EndAddress = code_size;
	unwindinfo->runtimeFunction.UnwindData = ((guchar*)&targetinfo->unwindInfo) - ((guchar*)code);
	
	memcpy (targetinfo, unwindinfo, sizeof (MonoUnwindInfo) - (sizeof (UNWIND_CODE) * MONO_MAX_UNWIND_CODES));
	
	codecount = unwindinfo->unwindInfo.CountOfCodes;
	if (codecount) {
		memcpy (&targetinfo->unwindInfo.UnwindCode[0], &unwindinfo->unwindInfo.UnwindCode[MONO_MAX_UNWIND_CODES-codecount], 
			sizeof (UNWIND_CODE) * unwindinfo->unwindInfo.CountOfCodes);
	}

	g_free (unwindinfo);
	*monoui = 0;

	RtlInstallFunctionTableCallback (((DWORD64)code) | 0x3, (DWORD64)code, code_size, MONO_GET_RUNTIME_FUNCTION_CALLBACK, code, NULL);
}

#endif

#if MONO_SUPPORT_TASKLETS
MonoContinuationRestore
mono_tasklets_arch_restore (void)
{
	static guint8* saved = NULL;
	guint8 *code, *start;
	int cont_reg = AMD64_R9; /* register usable on both call conventions */
	const guint kMaxCodeSize = 64;
	

	if (saved)
		return (MonoContinuationRestore)saved;
	code = start = (guint8 *)mono_global_codeman_reserve (kMaxCodeSize);
	/* the signature is: restore (MonoContinuation *cont, int state, MonoLMF **lmf_addr) */
	/* cont is in AMD64_ARG_REG1 ($rcx or $rdi)
	 * state is in AMD64_ARG_REG2 ($rdx or $rsi)
	 * lmf_addr is in AMD64_ARG_REG3 ($r8 or $rdx)
	 * We move cont to cont_reg since we need both rcx and rdi for the copy
	 * state is moved to $rax so it's setup as the return value and we can overwrite $rsi
 	 */
	amd64_mov_reg_reg (code, cont_reg, MONO_AMD64_ARG_REG1, 8);
	amd64_mov_reg_reg (code, AMD64_RAX, MONO_AMD64_ARG_REG2, 8);
	/* setup the copy of the stack */
	amd64_mov_reg_membase (code, AMD64_RCX, cont_reg, MONO_STRUCT_OFFSET (MonoContinuation, stack_used_size), sizeof (int));
	amd64_shift_reg_imm (code, X86_SHR, AMD64_RCX, 3);
	x86_cld (code);
	amd64_mov_reg_membase (code, AMD64_RSI, cont_reg, MONO_STRUCT_OFFSET (MonoContinuation, saved_stack), sizeof (gpointer));
	amd64_mov_reg_membase (code, AMD64_RDI, cont_reg, MONO_STRUCT_OFFSET (MonoContinuation, return_sp), sizeof (gpointer));
	amd64_prefix (code, X86_REP_PREFIX);
	amd64_movsl (code);

	/* now restore the registers from the LMF */
	amd64_mov_reg_membase (code, AMD64_RCX, cont_reg, MONO_STRUCT_OFFSET (MonoContinuation, lmf), 8);
	amd64_mov_reg_membase (code, AMD64_RBP, AMD64_RCX, MONO_STRUCT_OFFSET (MonoLMF, rbp), 8);
	amd64_mov_reg_membase (code, AMD64_RSP, AMD64_RCX, MONO_STRUCT_OFFSET (MonoLMF, rsp), 8);

#ifdef WIN32
	amd64_mov_reg_reg (code, AMD64_R14, AMD64_ARG_REG3, 8);
#else
	amd64_mov_reg_reg (code, AMD64_R12, AMD64_ARG_REG3, 8);
#endif

	/* state is already in rax */
	amd64_jump_membase (code, cont_reg, MONO_STRUCT_OFFSET (MonoContinuation, return_ip));
	g_assert ((code - start) <= kMaxCodeSize);

	mono_arch_flush_icache (start, code - start);
	mono_profiler_code_buffer_new (start, code - start, MONO_PROFILER_CODE_BUFFER_EXCEPTION_HANDLING, NULL);

	saved = start;
	return (MonoContinuationRestore)saved;
}
#endif

/*
 * mono_arch_setup_resume_sighandler_ctx:
 *
 *   Setup CTX so execution continues at FUNC.
 */
void
mono_arch_setup_resume_sighandler_ctx (MonoContext *ctx, gpointer func)
{
	/* 
	 * When resuming from a signal handler, the stack should be misaligned, just like right after
	 * a call.
	 */
	if ((((guint64)MONO_CONTEXT_GET_SP (ctx)) % 16) == 0)
		MONO_CONTEXT_SET_SP (ctx, (guint64)MONO_CONTEXT_GET_SP (ctx) - 8);
	MONO_CONTEXT_SET_IP (ctx, func);
}
