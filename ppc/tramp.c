/*
 * Create trampolines to invoke arbitrary functions.
 * 
 * Copyright (C) Radek Doulik
 * 
 */

#include "config.h"
#include <stdlib.h>
#include "ppc-codegen.h"
#include "mono/metadata/class.h"
#include "mono/metadata/tabledefs.h"
#include "mono/interpreter/interp.h"
#include "mono/metadata/appdomain.h"

#ifdef NEED_MPROTECT
#include <sys/mman.h>
#include <limits.h>    /* for PAGESIZE */
#ifndef PAGESIZE
#define PAGESIZE 4096
#endif
#endif

#define DEBUG(x)

/* gpointer
fake_func (gpointer (*callme)(gpointer), stackval *retval, void *this_obj, stackval *arguments)
{
	guint32 i = 0xc002becd;

	callme = (gpointer) 0x100fabcd;

	*(gpointer*)retval = (gpointer)(*callme) (arguments [0].data.p, arguments [1].data.p, arguments [2].data.p);
	*(gdouble*) retval = (gdouble)(*callme) (arguments [0].data.f);

	return (gpointer) (*callme) (((MonoType *)arguments [0]. data.p)->data.klass);
} */

#define MIN_CACHE_LINE 8

static void inline
flush_icache (guint8 *code, guint size)
{
	guint i;
	guint8 *p;

	p = code;
	for (i = 0; i < size; i += MIN_CACHE_LINE, p += MIN_CACHE_LINE) {
		asm ("dcbst 0,%0;" : : "r"(p) : "memory");
	}
	asm ("sync");
	p = code;
	for (i = 0; i < size; i += MIN_CACHE_LINE, p += MIN_CACHE_LINE) {
		asm ("icbi 0,%0; sync;" : : "r"(p) : "memory");
	}
	asm ("sync");
	asm ("isync");
}

#define NOT_IMPLEMENTED(x) \
                g_error ("FIXME: %s is not yet implemented. (trampoline)", x);

#define PROLOG_INS 8
#define CALL_INS   2
#define EPILOG_INS 6
#define MINIMAL_STACK_SIZE 5
#define FLOAT_REGS 8
#define GENERAL_REGS 8

static void inline
add_general (guint *gr, guint *stack_size, guint *code_size, gboolean simple)
{
	if (simple) {
		if (*gr >= GENERAL_REGS) {
			*stack_size += 4;
			*code_size += 8;    /* load from stack, save on stack */
		} else {
			*code_size += 4;    /* load from stack */
		}
	} else {
		if (*gr >= GENERAL_REGS - 1) {
			*stack_size += 8 + (*stack_size % 8);
			*code_size += 16;   /* 2x load from stack, 2x save to stack */
		} else {
			*code_size += 16;   /* 2x load from stack */
		}
		if ((*gr) && 1)
			(*gr) ++;
		(*gr) ++;
	}
	(*gr) ++;
}

static void inline
calculate_sizes (MonoMethodSignature *sig, guint *stack_size, guint *code_size, gboolean string_ctor)
{
	guint i, fr, gr;
	guint32 simpletype;

	fr = gr = 0;
	*stack_size = MINIMAL_STACK_SIZE*4;
	*code_size  = (PROLOG_INS + CALL_INS + EPILOG_INS)*4;

	if (sig->hasthis) {
		add_general (&gr, stack_size, code_size, TRUE);
	}

	for (i = 0; i < sig->param_count; ++i) {
		if (sig->params [i]->byref) {
			add_general (&gr, stack_size, code_size, TRUE);
			continue;
		}
		simpletype = sig->params [i]->type;
	enum_calc_size:
		switch (simpletype) {
		case MONO_TYPE_BOOLEAN:
		case MONO_TYPE_CHAR:
		case MONO_TYPE_I1:
		case MONO_TYPE_U1:
		case MONO_TYPE_I2:
		case MONO_TYPE_U2:
		case MONO_TYPE_I4:
		case MONO_TYPE_U4:
		case MONO_TYPE_I:
		case MONO_TYPE_U:
		case MONO_TYPE_PTR:
		case MONO_TYPE_CLASS:
		case MONO_TYPE_OBJECT:
		case MONO_TYPE_STRING:
			add_general (&gr, stack_size, code_size, TRUE);
			break;
		case MONO_TYPE_SZARRAY:
			add_general (&gr, stack_size, code_size, TRUE);
			*code_size += 4;
			break;
		case MONO_TYPE_VALUETYPE:
			if (sig->params [i]->data.klass->enumtype) {
				simpletype = sig->params [i]->data.klass->enum_basetype->type;
				goto enum_calc_size;
			}
			if (mono_class_value_size (sig->params [i]->data.klass, NULL) != 4)
				g_error ("can only marshal enums, not generic structures (size: %d)",
					 mono_class_value_size (sig->params [i]->data.klass, NULL));
			add_general (&gr, stack_size, code_size, TRUE);
			*code_size += 4;
			break;
		case MONO_TYPE_I8:
			add_general (&gr, stack_size, code_size, FALSE);
			break;
		case MONO_TYPE_R4:
		case MONO_TYPE_R8:
			if (fr < 7) {
				*code_size += 4;
				fr ++;
			} else {
				NOT_IMPLEMENTED ("R8 arg");
			}
			break;
		default:
			g_error ("Can't trampoline 0x%x", sig->params [i]->type);
		}
	}

	if (sig->ret->byref || string_ctor) {
		*code_size += 8;
	} else {
		simpletype = sig->ret->type;
enum_retvalue:
		switch (simpletype) {
		case MONO_TYPE_BOOLEAN:
		case MONO_TYPE_I1:
		case MONO_TYPE_U1:
		case MONO_TYPE_I2:
		case MONO_TYPE_U2:
		case MONO_TYPE_CHAR:
		case MONO_TYPE_I4:
		case MONO_TYPE_U4:
		case MONO_TYPE_I:
		case MONO_TYPE_U:
		case MONO_TYPE_CLASS:
		case MONO_TYPE_OBJECT:
		case MONO_TYPE_R4:
		case MONO_TYPE_R8:
		case MONO_TYPE_SZARRAY:
		case MONO_TYPE_ARRAY:
		case MONO_TYPE_STRING:
			*code_size += 8;
			break;
		case MONO_TYPE_I8:
			*code_size += 12;
			break;
		case MONO_TYPE_VALUETYPE:
			if (sig->ret->data.klass->enumtype) {
				simpletype = sig->ret->data.klass->enum_basetype->type;
				goto enum_retvalue;
			}
			NOT_IMPLEMENTED ("valuetype");
			break;
		case MONO_TYPE_VOID:
			break;
		default:
			g_error ("Can't handle as return value 0x%x", sig->ret->type);
		}
	}

	/* align stack size to 16 */
	DEBUG (printf ("      stack size: %d (%d)\n       code size: %d\n", (*stack_size + 15) & ~15, *stack_size, *code_size));
	*stack_size = (*stack_size + 15) & ~15;

}

static inline guint8 *
emit_prolog (guint8 *p, MonoMethodSignature *sig, guint stack_size)
{
	/* function prolog */
	ppc_stwu (p, ppc_r1, -stack_size, ppc_r1);     /* sp      <--- sp - stack_size, sp[0] <---- sp save sp, alloc stack */
	ppc_mflr (p, ppc_r0);                          /* r0      <--- LR */
	ppc_stw  (p, ppc_r31, stack_size - 4, ppc_r1); /* sp[+4]  <--- r31     save r31 */
	ppc_stw  (p, ppc_r0, stack_size + 4, ppc_r1);  /* sp[-4]  <--- LR      save return address for "callme" */
	ppc_mr   (p, ppc_r31, ppc_r1);                 /* r31     <--- sp */

	ppc_mr   (p, ppc_r12, ppc_r6);                        /* keep "arguments" in register */
	ppc_mr   (p, ppc_r0, ppc_r3);                         /* keep "callme" in register */

	ppc_stw  (p, ppc_r4, stack_size - 12, ppc_r31);               /* preserve "retval", sp[+8] */

	return p;
}

#define ARG_BASE ppc_r12
#define SAVE_4_IN_GENERIC_REGISTER \
			if (gr < GENERAL_REGS) { \
				ppc_lwz  (p, ppc_r3 + gr, i*16, ARG_BASE); \
				gr ++; \
			} else { \
				ppc_lwz  (p, ppc_r11, i*16, ARG_BASE); \
				ppc_stw  (p, ppc_r11, stack_par_pos, ppc_r1); \
                                stack_par_pos += 4; \
			}

inline static guint8*
emit_save_parameters (guint8 *p, MonoMethodSignature *sig, guint stack_size)
{
	guint i, fr, gr, act_strs, stack_par_pos;
	guint32 simpletype;

	fr = gr = 0;
	act_strs = 0;
	stack_par_pos = 8;

	if (sig->hasthis) {
		ppc_mr (p, ppc_r3, ppc_r5);
		gr ++;
	}

	act_strs = 0;
	for (i = 0; i < sig->param_count; ++i) {
		if (sig->params [i]->byref) {
			SAVE_4_IN_GENERIC_REGISTER;
			continue;
		}
		simpletype = sig->params [i]->type;
	enum_calc_size:
		switch (simpletype) {
		case MONO_TYPE_BOOLEAN:
		case MONO_TYPE_I1:
		case MONO_TYPE_U1:
		case MONO_TYPE_I2:
		case MONO_TYPE_U2:
		case MONO_TYPE_CHAR:
		case MONO_TYPE_I4:
		case MONO_TYPE_U4:
		case MONO_TYPE_I:
		case MONO_TYPE_U:
		case MONO_TYPE_PTR:
		case MONO_TYPE_CLASS:
		case MONO_TYPE_OBJECT:
		case MONO_TYPE_STRING:
		case MONO_TYPE_SZARRAY:
			SAVE_4_IN_GENERIC_REGISTER;
			break;
			/* g_warning ("untested marshaling\n");
				if (gr < GENERAL_REGS) {
					ppc_lwz  (p, ppc_r3 + gr, i*16, ARG_BASE);
					ppc_lwz  (p, ppc_r3 + gr, G_STRUCT_OFFSET (MonoArray, vector), ppc_r3 + gr);
					gr ++;
				} else {
					NOT_IMPLEMENTED ("save marshalled SZARRAY on stack");
				}
				break; */
		case MONO_TYPE_VALUETYPE:
			if (sig->params [i]->data.klass->enumtype) {
				simpletype = sig->params [i]->data.klass->enum_basetype->type;
				goto enum_calc_size;
			}
			if (mono_class_value_size (sig->params [i]->data.klass, NULL) != 4)
				g_error ("can only marshal enums, not generic structures (size: %d)",
					 mono_class_value_size (sig->params [i]->data.klass, NULL));
			if (gr < GENERAL_REGS) {
				ppc_lwz  (p, ppc_r3 + gr, i*16, ARG_BASE);
				ppc_lwz  (p, ppc_r3 + gr, 0, ppc_r3 + gr);
				gr ++;
			} else {
				NOT_IMPLEMENTED ("save value type on stack");
			}
			break;
		case MONO_TYPE_I8:
			if (gr < 7) {
				if (gr & 1)
					gr ++;
				ppc_lwz  (p, ppc_r3 + gr, i*16, ARG_BASE);
				gr ++;
				ppc_lwz  (p, ppc_r3 + gr, i*16 + 4, ARG_BASE);
				gr ++;
			} else {
				NOT_IMPLEMENTED ("i8 on stack");
			}
			break;
		case MONO_TYPE_R4:
			if (fr < 7) {
				ppc_lfs  (p, ppc_f1 + fr, i*16, ARG_BASE);
				fr ++;
			} else {
				NOT_IMPLEMENTED ("r4 on stack");
			}
			break;
		case MONO_TYPE_R8:
			if (fr < 7) {
				ppc_lfd  (p, ppc_f1 + fr, i*16, ARG_BASE);
				fr ++;
			} else {
				NOT_IMPLEMENTED ("r8 on stack");
			}
			break;
		default:
			g_error ("Can't trampoline 0x%x", sig->params [i]->type);
		}
	}

	return p;
}

static inline guint8 *
alloc_code_memory (guint code_size)
{
	guint8 *p;

#ifdef NEED_MPROTECT
	p = g_malloc (code_size + PAGESIZE - 1);

	/* Align to a multiple of PAGESIZE, assumed to be a power of two */
	p = (char *)(((int) p + PAGESIZE-1) & ~(PAGESIZE-1));
#else
	p = g_malloc (code_size);
#endif
	DEBUG (printf ("           align: %p (%d)\n", p, (guint)p % 4));

	return p;
}

/* static MonoString*
mono_string_new_wrapper (const char *text)
{
	return text ? mono_string_new (mono_domain_get (), text) : NULL;
} */

static inline guint8 *
emit_call_and_store_retval (guint8 *p, MonoMethodSignature *sig, guint stack_size, gboolean string_ctor)
{
	guint32 simpletype;

	/* call "callme" */
	ppc_mtlr (p, ppc_r0);
	ppc_blrl (p);

	/* get return value */
	if (sig->ret->byref || string_ctor) {
		ppc_lwz  (p, ppc_r9, stack_size - 12, ppc_r31);        /* load "retval" address */
		ppc_stw  (p, ppc_r3, 0, ppc_r9);                       /* save return value (r3) to "retval" */
	} else {
		simpletype = sig->ret->type;
enum_retvalue:
		switch (simpletype) {
		case MONO_TYPE_BOOLEAN:
		case MONO_TYPE_I1:
		case MONO_TYPE_U1:
			ppc_lwz  (p, ppc_r9, stack_size - 12, ppc_r31);        /* load "retval" address */
			ppc_stb  (p, ppc_r3, 0, ppc_r9);                       /* save return value (r3) to "retval" */
			break;
		case MONO_TYPE_I2:
		case MONO_TYPE_U2:
		case MONO_TYPE_CHAR:
			ppc_lwz  (p, ppc_r9, stack_size - 12, ppc_r31);        /* load "retval" address */
			ppc_sth  (p, ppc_r3, 0, ppc_r9);                       /* save return value (r3) to "retval" */
			break;
		case MONO_TYPE_I4:
		case MONO_TYPE_U4:
		case MONO_TYPE_I:
		case MONO_TYPE_U:
		case MONO_TYPE_CLASS:
		case MONO_TYPE_OBJECT:
		case MONO_TYPE_SZARRAY:
		case MONO_TYPE_ARRAY:
		case MONO_TYPE_STRING:
			ppc_lwz  (p, ppc_r9, stack_size - 12, ppc_r31);        /* load "retval" address */
			ppc_stw  (p, ppc_r3, 0, ppc_r9);                       /* save return value (r3) to "retval" */
			break;
		case MONO_TYPE_R4:
			ppc_lwz  (p, ppc_r9, stack_size - 12, ppc_r31);        /* load "retval" address */
			ppc_stfs (p, ppc_f1, 0, ppc_r9);                       /* save return value (f1) to "retval" */
			break;
		case MONO_TYPE_R8:
			ppc_lwz  (p, ppc_r9, stack_size - 12, ppc_r31);        /* load "retval" address */
			ppc_stfd (p, ppc_f1, 0, ppc_r9);                       /* save return value (f1) to "retval" */
			break;
		case MONO_TYPE_I8:
			ppc_lwz  (p, ppc_r9, stack_size - 12, ppc_r31);        /* load "retval" address */
			ppc_stw  (p, ppc_r3, 0, ppc_r9);                       /* save return value (r3) to "retval" */
			ppc_stw  (p, ppc_r4, 4, ppc_r9);                       /* save return value (r3) to "retval" */
			break;
		case MONO_TYPE_VALUETYPE:
			if (sig->ret->data.klass->enumtype) {
				simpletype = sig->ret->data.klass->enum_basetype->type;
				goto enum_retvalue;
			}
			NOT_IMPLEMENTED ("retval valuetype");
			break;
		case MONO_TYPE_VOID:
			break;
		default:
			g_error ("Can't handle as return value 0x%x", sig->ret->type);
		}
	}

	return p;
}

static inline guint8 *
emit_epilog (guint8 *p, MonoMethodSignature *sig, guint stack_size)
{
	/* function epilog */
	ppc_lwz  (p, ppc_r11, 0,  ppc_r1);        /* r11     <--- sp[0]   load backchain from caller's function */
	ppc_lwz  (p, ppc_r0, 4, ppc_r11);         /* r0      <--- r11[4]  load return address */
	ppc_mtlr (p, ppc_r0);                     /* LR      <--- r0      set return address */
	ppc_lwz  (p, ppc_r31, -4, ppc_r11);       /* r31     <--- r11[-4] restore r31 */
	ppc_mr   (p, ppc_r1, ppc_r11);            /* sp      <--- r11     restore stack */
	ppc_blr  (p);                             /* return */

	return p;
}

MonoPIFunc
mono_create_trampoline (MonoMethodSignature *sig, gboolean string_ctor)
{
	guint8 *p, *code_buffer;
	guint stack_size, code_size;

	DEBUG (printf ("\nPInvoke [start emiting]\n"));
	calculate_sizes (sig, &stack_size, &code_size, string_ctor);

	p = code_buffer = alloc_code_memory (code_size);
	p = emit_prolog (p, sig, stack_size);
	p = emit_save_parameters (p, sig, stack_size);
	p = emit_call_and_store_retval (p, sig, stack_size, string_ctor);
	p = emit_epilog (p, sig, stack_size);

	/* {
		guchar *cp;
		printf (".text\n.align 4\n.globl main\n.type main,@function\nmain:\n");
		for (cp = code_buffer; cp < p; cp++) {
			printf (".byte 0x%x\n", *cp);
		}
		} */

#ifdef NEED_MPROTECT
	if (mprotect (code_buffer, 1024, PROT_READ | PROT_WRITE | PROT_EXEC)) {
		g_error ("Cannot mprotect trampoline\n");
	}
#endif

	DEBUG (printf ("emited code size: %d\n", p - code_buffer));
	flush_icache (code_buffer, p - code_buffer);

	DEBUG (printf ("PInvoke [end emiting]\n"));

	return (MonoPIFunc) code_buffer;
	/* return fake_func; */
}


#define MINV_POS  8   /* MonoInvocation structure offset on stack */
#define STACK_POS (MINV_POS - sizeof (stackval) * sig->param_count)
#define OBJ_POS   8
#define TYPE_OFFSET (G_STRUCT_OFFSET (stackval, type))

/*
 * Returns a pointer to a native function that can be used to
 * call the specified method.
 * The function created will receive the arguments according
 * to the call convention specified in the method.
 * This function works by creating a MonoInvocation structure,
 * filling the fields in and calling ves_exec_method on it.
 * Still need to figure out how to handle the exception stuff
 * across the managed/unmanaged boundary.
 */
void *
mono_create_method_pointer (MonoMethod *method)
{
	MonoMethodSignature *sig;
	MonoJitInfo *ji;
	guint8 *p, *code_buffer;
	guint i, align = 0, code_size, stack_size, stackval_arg_pos, local_pos, local_start, reg_param, stack_param,
		this_flag;
	guint32 simpletype;

	code_size = 1024;
	stack_size = 1024;
	stack_param = 0;

	sig = method->signature;

	p = code_buffer = g_malloc (code_size);

	DEBUG (printf ("\nDelegate [start emiting] %s\n", method->name));

	/* prolog */
	ppc_stwu (p, ppc_r1, -stack_size, ppc_r1);     /* sp      <--- sp - stack_size, sp[0] <---- sp save sp, alloc stack */
	ppc_mflr (p, ppc_r0);                          /* r0      <--- LR */
	ppc_stw  (p, ppc_r31, stack_size - 4, ppc_r1); /* sp[+4]  <--- r31     save r31 */
	ppc_stw  (p, ppc_r0, stack_size + 4, ppc_r1);  /* sp[-4]  <--- LR      save return address for "callme" */
	ppc_mr   (p, ppc_r31, ppc_r1);                 /* r31     <--- sp */

	/* let's fill MonoInvocation */
	/* first zero some fields */
	ppc_li   (p, ppc_r0, 0);
	ppc_stw  (p, ppc_r0, (MINV_POS + G_STRUCT_OFFSET (MonoInvocation, ex)), ppc_r31);
	ppc_stw  (p, ppc_r0, (MINV_POS + G_STRUCT_OFFSET (MonoInvocation, ex_handler)), ppc_r31);
	ppc_stw  (p, ppc_r0, (MINV_POS + G_STRUCT_OFFSET (MonoInvocation, child)), ppc_r31);
	ppc_stw  (p, ppc_r0, (MINV_POS + G_STRUCT_OFFSET (MonoInvocation, parent)), ppc_r31);

	/* set method pointer */
	ppc_lis  (p, ppc_r0,     (guint32) method >> 16);
	ppc_ori  (p, ppc_r0, ppc_r0, (guint32) method & 0xffff);
	ppc_stw  (p, ppc_r0, (MINV_POS + G_STRUCT_OFFSET (MonoInvocation, method)), ppc_r31);

	local_start = local_pos = MINV_POS + sizeof (MonoInvocation) + (sig->param_count + 1) * sizeof (stackval);

	if (sig->hasthis) {
		ppc_stw  (p, ppc_r3, (MINV_POS + G_STRUCT_OFFSET (MonoInvocation, obj)), ppc_r31);
		reg_param = 1;
	} else if (sig->param_count) {
		DEBUG (printf ("save r%d\n", 3));
		ppc_stw  (p, ppc_r3, local_pos, ppc_r31);
		local_pos += 4;
		reg_param = 0;
	}

	this_flag = (sig->hasthis ? 1 : 0);
	if (sig->param_count) {
		gint save_count = MAX (3, MIN (8, sig->param_count - 1));
		for (i = reg_param; i < save_count; i ++) {
			ppc_stw (p, ppc_r4 + i, local_pos, ppc_r31);
			local_pos += 4;
			DEBUG (printf ("save r%d\n", 4 + i));
		}
	}

	/* set MonoInvocation::stack_args */
	stackval_arg_pos = MINV_POS + sizeof (MonoInvocation);
	ppc_addi (p, ppc_r0, ppc_r31, stackval_arg_pos);
	ppc_stw  (p, ppc_r0, (MINV_POS + G_STRUCT_OFFSET (MonoInvocation, stack_args)), ppc_r31);

	/* add stackval arguments */
	for (i = 0; i < sig->param_count; ++i) {
		if (reg_param < 8) {
			ppc_addi (p, ppc_r5, ppc_r31, local_start + (reg_param - this_flag)*4);
			reg_param ++;
		} else {
			ppc_addi (p, ppc_r5, stack_size + 8 + stack_param, ppc_r31);
			stack_param ++;
		}
		ppc_lis  (p, ppc_r3, (guint32) sig->params [i] >> 16);
		ppc_addi (p, ppc_r4, ppc_r31, stackval_arg_pos);

		ppc_ori  (p, ppc_r3, ppc_r3, (guint32) sig->params [i] & 0xffff);
		ppc_lis  (p, ppc_r0, (guint32) stackval_from_data >> 16);
		ppc_li   (p, ppc_r6, sig->pinvoke);
		ppc_ori  (p, ppc_r0, ppc_r0, (guint32) stackval_from_data & 0xffff);
		ppc_mtlr (p, ppc_r0);
		ppc_blrl (p);

		/* fixme: alignment */
		DEBUG (printf ("arg_pos %d --> ", stackval_arg_pos));
		if (sig->pinvoke)
			stackval_arg_pos += 4*mono_type_native_stack_size (sig->params [i], &align);
		else
			stackval_arg_pos += 4*mono_type_stack_size (sig->params [i], &align);
		DEBUG (printf ("%d\n", stackval_arg_pos));
	}

	/* return value storage */
	if (sig->param_count) {
		ppc_addi (p, ppc_r0, ppc_r31, stackval_arg_pos);
	}
	ppc_stw  (p, ppc_r0, (MINV_POS + G_STRUCT_OFFSET (MonoInvocation, retval)), ppc_r31);

	/* call ves_exec_method */
	ppc_lis  (p, ppc_r0,     (guint32) ves_exec_method >> 16);
	ppc_addi (p, ppc_r3, ppc_r31, MINV_POS);
	ppc_ori  (p, ppc_r0, ppc_r0, (guint32) ves_exec_method & 0xffff);
	ppc_mtlr (p, ppc_r0);
	ppc_blrl (p);

	/* move retval from stackval to proper place (r3/r4/...) */
	if (sig->ret->byref) {
		DEBUG (printf ("ret by ref\n"));
		ppc_lwz (p, ppc_r3, stackval_arg_pos, ppc_r31);
	} else {
	enum_retvalue:
		switch (sig->ret->type) {
		case MONO_TYPE_VOID:
			break;
		case MONO_TYPE_BOOLEAN:
		case MONO_TYPE_I1:
		case MONO_TYPE_U1:
			ppc_lbz (p, ppc_r3, stackval_arg_pos, ppc_r31);
			break;
		case MONO_TYPE_I2:
		case MONO_TYPE_U2:
			ppc_lhz (p, ppc_r3, stackval_arg_pos, ppc_r31);
			break;
		case MONO_TYPE_I4:
		case MONO_TYPE_U4:
		case MONO_TYPE_I:
		case MONO_TYPE_U:
		case MONO_TYPE_OBJECT:
		case MONO_TYPE_STRING:
		case MONO_TYPE_CLASS:
			ppc_lwz (p, ppc_r3, stackval_arg_pos, ppc_r31);
			break;
		case MONO_TYPE_I8:
			ppc_lwz (p, ppc_r3, stackval_arg_pos, ppc_r31);
			ppc_lwz (p, ppc_r4, stackval_arg_pos + 4, ppc_r31);
			break;
		case MONO_TYPE_R4:
			ppc_lfs (p, ppc_f1, stackval_arg_pos, ppc_r31);
			break;
		case MONO_TYPE_R8:
			ppc_lfd (p, ppc_f1, stackval_arg_pos, ppc_r31);
			break;
		case MONO_TYPE_VALUETYPE:
			if (sig->ret->data.klass->enumtype) {
				simpletype = sig->ret->data.klass->enum_basetype->type;
				goto enum_retvalue;
			}
			NOT_IMPLEMENTED ("value type as ret val from delegate");
			break;
		default:
			g_error ("Type 0x%x not handled yet in thunk creation", sig->ret->type);
			break;
		}
	}

	/* epilog */
	ppc_lwz  (p, ppc_r11, 0,  ppc_r1);        /* r11     <--- sp[0]   load backchain from caller's function */
	ppc_lwz  (p, ppc_r0, 4, ppc_r11);         /* r0      <--- r11[4]  load return address */
	ppc_mtlr (p, ppc_r0);                     /* LR      <--- r0      set return address */
	ppc_lwz  (p, ppc_r31, -4, ppc_r11);       /* r31     <--- r11[-4] restore r31 */
	ppc_mr   (p, ppc_r1, ppc_r11);            /* sp      <--- r11     restore stack */
	ppc_blr  (p);                             /* return */

	DEBUG (printf ("emited code size: %d\n", p - code_buffer));
	flush_icache (code_buffer, p - code_buffer);

	DEBUG (printf ("Delegate [end emiting]\n"));

	ji = g_new0 (MonoJitInfo, 1);
	ji->method = method;
	ji->code_size = p - code_buffer;
	ji->code_start = code_buffer;

	mono_jit_info_table_add (mono_root_domain, ji);

	return ji->code_start;
}
