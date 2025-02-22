// SPDX-FileCopyrightText: 1989-1999 Free Software Foundation, Inc.
// SPDX-License-Identifier: GPL-2.0-or-later

/* Demangler for GNU C++
   Copyright 1989, 91, 94, 95, 96, 97, 98, 1999 Free Software Foundation, Inc.
   Written by James Clark (jjc@jclark.uucp)
   Rewritten by Fred Fish (fnf@cygnus.com) for ARM and Lucid demangling
   Modified by Satish Pai (pai@apollo.hp.com) for HP demangling

This file is part of the libiberty library.
Libiberty is free software; you can redistribute it and/or
modify it under the terms of the GNU Library General Public
License as published by the Free Software Foundation; either
version 2 of the License, or (at your option) any later version.

Libiberty is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
Library General Public License for more details.

You should have received a copy of the GNU Library General Public
License along with libiberty; see the file COPYING.LIB.  If
not, write to the Free Software Foundation, Inc., 59 Temple Place - Suite 330,
Boston, MA 02111-1307, USA.  */

/* This file exports two functions; cplus_mangle_opname and cplus_demangle_v2.

   This file imports malloc and realloc, which are like malloc and
   realloc except that they generate a fatal error if there is no
   available memory.  */

/* This file lives in both GCC and libiberty.  When making changes, please
   try not to break either.  */

#include <ctype.h>
#include <sys/types.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

#define UT32_MAX 0xffffffffu

#include "cplus-dem.h"
#undef CURRENT_DEMANGLING_STYLE
#define CURRENT_DEMANGLING_STYLE work->options

#define min(X, Y) (((X) < (Y)) ? (X) : (Y))

static const char *mystrstr PARAMS((const char *, const char *));

static const char *
	mystrstr(const char *s1, const char *s2)
{
	register const char *p = s1;
	register int len = strlen(s2);

	for (; (p = strchr(p, *s2)) != 0; p++) {
		if (strncmp(p, s2, len) == 0) {
			return (p);
		}
	}
	return (0);
}

/* In order to allow a single demangler executable to demangle strings
   using various common values of CPLUS_MARKER, as well as any specific
   one set at compile time, we maintain a string containing all the
   commonly used ones, and check to see if the marker we are looking for
   is in that string.  CPLUS_MARKER is usually '$' on systems where the
   assembler can deal with that.  Where the assembler can't, it's usually
   '.' (but on many systems '.' is used for other things).  We put the
   current defined CPLUS_MARKER first (which defaults to '$'), followed
   by the next most common value, followed by an explicit '$' in case
   the value of CPLUS_MARKER is not '$'.

   We could avoid this if we could just get g++ to tell us what the actual
   cplus marker character is as part of the debug information, perhaps by
   ensuring that it is the character that terminates the gcc<n>_compiled
   marker symbol (FIXME).  */

#if !defined(CPLUS_MARKER)
#define CPLUS_MARKER '$'
#endif

enum demangling_styles current_demangling_style = gnu_demangling;

static char cplus_markers[] = { CPLUS_MARKER, '.', '$', '\0' };

static char char_str[2] = { '\000', '\000' };

typedef struct string /* Beware: these aren't required to be */
{ /*  '\0' terminated.  */
	char *b; /* pointer to start of string */
	char *p; /* pointer after last character */
	char *e; /* pointer after end of allocated space */
} string;

/* Stuff that is shared between sub-routines.
   Using a shared structure allows cplus_demangle_v2 to be reentrant.  */

struct work_stuff {
	int options;
	char **typevec;
	char **ktypevec;
	char **btypevec;
	int numk;
	int numb;
	int ksize;
	int bsize;
	int ntypes;
	int typevec_size;
	int constructor;
	int destructor;
	int static_type; /* A static member function */
	int temp_start; /* index in demangled to start of template args */
	int type_quals; /* The type qualifiers.  */
	int dllimported; /* Symbol imported from a PE DLL */
	char **tmpl_argvec; /* Template function arguments. */
	int ntmpl_args; /* The number of template function arguments. */
	int forgetting_types; /* Nonzero if we are not remembering the types
				 we see.  */
	string *previous_argument; /* The last function argument demangled.  */
	int nrepeats; /* The number of times to repeat the previous
			 argument.  */
};

#define PRINT_ANSI_QUALIFIERS (work->options & DMGL_ANSI)
#define PRINT_ARG_TYPES       (work->options & DMGL_PARAMS)

static const struct optable {
	const char *in;
	const char *out;
	int flags;
} optable[] = {
	{ "nw", " new", DMGL_ANSI }, /* new (1.92,	 ansi) */
	{ "dl", " delete", DMGL_ANSI }, /* new (1.92,	 ansi) */
	{ "new", " new", 0 }, /* old (1.91,	 and 1.x) */
	{ "delete", " delete", 0 }, /* old (1.91,	 and 1.x) */
	{ "vn", " new []", DMGL_ANSI }, /* GNU, pending ansi */
	{ "vd", " delete []", DMGL_ANSI }, /* GNU, pending ansi */
	{ "as", "=", DMGL_ANSI }, /* ansi */
	{ "ne", "!=", DMGL_ANSI }, /* old, ansi */
	{ "eq", "==", DMGL_ANSI }, /* old,	ansi */
	{ "ge", ">=", DMGL_ANSI }, /* old,	ansi */
	{ "gt", ">", DMGL_ANSI }, /* old,	ansi */
	{ "le", "<=", DMGL_ANSI }, /* old,	ansi */
	{ "lt", "<", DMGL_ANSI }, /* old,	ansi */
	{ "plus", "+", 0 }, /* old */
	{ "pl", "+", DMGL_ANSI }, /* ansi */
	{ "apl", "+=", DMGL_ANSI }, /* ansi */
	{ "minus", "-", 0 }, /* old */
	{ "mi", "-", DMGL_ANSI }, /* ansi */
	{ "ami", "-=", DMGL_ANSI }, /* ansi */
	{ "mult", "*", 0 }, /* old */
	{ "ml", "*", DMGL_ANSI }, /* ansi */
	{ "amu", "*=", DMGL_ANSI }, /* ansi (ARM/Lucid) */
	{ "aml", "*=", DMGL_ANSI }, /* ansi (GNU/g++) */
	{ "convert", "+", 0 }, /* old (unary +) */
	{ "negate", "-", 0 }, /* old (unary -) */
	{ "trunc_mod", "%", 0 }, /* old */
	{ "md", "%", DMGL_ANSI }, /* ansi */
	{ "amd", "%=", DMGL_ANSI }, /* ansi */
	{ "trunc_div", "/", 0 }, /* old */
	{ "dv", "/", DMGL_ANSI }, /* ansi */
	{ "adv", "/=", DMGL_ANSI }, /* ansi */
	{ "truth_andif", "&&", 0 }, /* old */
	{ "aa", "&&", DMGL_ANSI }, /* ansi */
	{ "truth_orif", "||", 0 }, /* old */
	{ "oo", "||", DMGL_ANSI }, /* ansi */
	{ "truth_not", "!", 0 }, /* old */
	{ "nt", "!", DMGL_ANSI }, /* ansi */
	{ "postincrement", "++", 0 }, /* old */
	{ "pp", "++", DMGL_ANSI }, /* ansi */
	{ "postdecrement", "--", 0 }, /* old */
	{ "mm", "--", DMGL_ANSI }, /* ansi */
	{ "bit_ior", "|", 0 }, /* old */
	{ "or", "|", DMGL_ANSI }, /* ansi */
	{ "aor", "|=", DMGL_ANSI }, /* ansi */
	{ "bit_xor", "^", 0 }, /* old */
	{ "er", "^", DMGL_ANSI }, /* ansi */
	{ "aer", "^=", DMGL_ANSI }, /* ansi */
	{ "bit_and", "&", 0 }, /* old */
	{ "ad", "&", DMGL_ANSI }, /* ansi */
	{ "aad", "&=", DMGL_ANSI }, /* ansi */
	{ "bit_not", "~", 0 }, /* old */
	{ "co", "~", DMGL_ANSI }, /* ansi */
	{ "call", "()", 0 }, /* old */
	{ "cl", "()", DMGL_ANSI }, /* ansi */
	{ "alshift", "<<", 0 }, /* old */
	{ "ls", "<<", DMGL_ANSI }, /* ansi */
	{ "als", "<<=", DMGL_ANSI }, /* ansi */
	{ "arshift", ">>", 0 }, /* old */
	{ "rs", ">>", DMGL_ANSI }, /* ansi */
	{ "ars", ">>=", DMGL_ANSI }, /* ansi */
	{ "component", "->", 0 }, /* old */
	{ "pt", "->", DMGL_ANSI }, /* ansi; Lucid C++ form */
	{ "rf", "->", DMGL_ANSI }, /* ansi; ARM/GNU form */
	{ "indirect", "*", 0 }, /* old */
	{ "method_call", "->()", 0 }, /* old */
	{ "addr", "&", 0 }, /* old (unary &) */
	{ "array", "[]", 0 }, /* old */
	{ "vc", "[]", DMGL_ANSI }, /* ansi */
	{ "compound", ", ", 0 }, /* old */
	{ "cm", ", ", DMGL_ANSI }, /* ansi */
	{ "cond", "?:", 0 }, /* old */
	{ "cn", "?:", DMGL_ANSI }, /* pseudo-ansi */
	{ "max", ">?", 0 }, /* old */
	{ "mx", ">?", DMGL_ANSI }, /* pseudo-ansi */
	{ "min", "<?", 0 }, /* old */
	{ "mn", "<?", DMGL_ANSI }, /* pseudo-ansi */
	{ "nop", "", 0 }, /* old (for operator=) */
	{ "rm", "->*", DMGL_ANSI }, /* ansi */
	{ "sz", "sizeof ", DMGL_ANSI } /* pseudo-ansi */
};

/* These values are used to indicate the various type varieties.
   They are all non-zero so that they can be used as `success'
   values.  */
typedef enum type_kind_t {
	tk_none,
	tk_pointer,
	tk_reference,
	tk_integral,
	tk_bool,
	tk_char,
	tk_real
} type_kind_t;

#define STRING_EMPTY(str) ((str)->b == (str)->p)
#define PREPEND_BLANK(str) \
	{ \
		if (!STRING_EMPTY(str)) \
			string_prepend(str, " "); \
	}
#define APPEND_BLANK(str) \
	{ \
		if (!STRING_EMPTY(str)) \
			string_append(str, " "); \
	}
#define LEN_STRING(str) ((STRING_EMPTY(str)) ? 0 : ((str)->p - (str)->b))

/* The scope separator appropriate for the language being demangled.  */

#define SCOPE_STRING(work) ((work->options & DMGL_JAVA) ? "." : "::")

#define ARM_VTABLE_STRING "__vtbl__" /* Lucid/ARM virtual table prefix */
#define ARM_VTABLE_STRLEN 8 /* strlen (ARM_VTABLE_STRING) */

/* Prototypes for local functions */

static char *
	mop_up PARAMS((struct work_stuff *, string *, int));

static void
	squangle_mop_up PARAMS((struct work_stuff *));

#if 0
static int
demangle_method_args PARAMS ((struct work_stuff *, const char **, string *));
#endif

static char *
	internal_cplus_demangle PARAMS((struct work_stuff *, const char *));

static int
	demangle_template_template_parm PARAMS((struct work_stuff * work,
		const char **, string *));

static int
	demangle_template PARAMS((struct work_stuff * work, const char **, string *,
		string *, int, int));

static int
	arm_pt PARAMS((struct work_stuff *, const char *, int, const char **,
		const char **));

static int
	demangle_class_name PARAMS((struct work_stuff *, const char **, string *));

static int
	demangle_qualified PARAMS((struct work_stuff *, const char **, string *,
		int, int));

static int
	demangle_class PARAMS((struct work_stuff *, const char **, string *));

static int
	demangle_fund_type PARAMS((struct work_stuff *, const char **, string *));

static int
	demangle_signature PARAMS((struct work_stuff *, const char **, string *));

static int
	demangle_prefix PARAMS((struct work_stuff *, const char **, string *));

static int
	gnu_special PARAMS((struct work_stuff *, const char **, string *));

static int
	arm_special PARAMS((const char **, string *));

static void
	string_need PARAMS((string *, int));

static void
	string_delete PARAMS((string *));

static void
	string_init PARAMS((string *));

static void
	string_clear PARAMS((string *));

#if 0
static int
string_empty PARAMS ((string *));
#endif

static void
	string_append PARAMS((string *, const char *));

static void
	string_appends PARAMS((string *, string *));

static void
	string_appendn PARAMS((string *, const char *, int));

static void
	string_prepend PARAMS((string *, const char *));

static void
	string_prependn PARAMS((string *, const char *, int));

static int
	get_count PARAMS((const char **, int *));

static int
	consume_count PARAMS((const char **));

static int
	consume_count_with_underscores PARAMS((const char **));

static int
	demangle_args PARAMS((struct work_stuff *, const char **, string *));

static int
	demangle_nested_args PARAMS((struct work_stuff *, const char **, string *));

static int
	do_type PARAMS((struct work_stuff *, const char **, string *));

static int
	do_arg PARAMS((struct work_stuff *, const char **, string *));

static void
	demangle_function_name PARAMS((struct work_stuff *, const char **, string *,
		const char *));

static void
	remember_type PARAMS((struct work_stuff *, const char *, int));

static void
	remember_Btype PARAMS((struct work_stuff *, const char *, int, int));

static int
	register_Btype PARAMS((struct work_stuff *));

static void
	remember_Ktype PARAMS((struct work_stuff *, const char *, int));

static void
	forget_types PARAMS((struct work_stuff *));

static void
	forget_B_and_K_types PARAMS((struct work_stuff *));

static void
	string_prepends PARAMS((string *, string *));

static int
	demangle_template_value_parm PARAMS((struct work_stuff *, const char **,
		string *, type_kind_t));

static int
	do_hpacc_template_const_value PARAMS((struct work_stuff *, const char **, string *));

static int
	do_hpacc_template_literal PARAMS((struct work_stuff *, const char **, string *));

static int
	snarf_numeric_literal PARAMS((const char **, string *));

/* There is a TYPE_QUAL value for each type qualifier.  They can be
   combined by bitwise-or to form the complete set of qualifiers for a
   type.  */

#define TYPE_UNQUALIFIED   0x0
#define TYPE_QUAL_CONST    0x1
#define TYPE_QUAL_VOLATILE 0x2
#define TYPE_QUAL_RESTRICT 0x4

static int
	code_for_qualifier PARAMS((int));

static const char *
	qualifier_string PARAMS((int));

static const char *
	demangle_qualifier PARAMS((int));

#define overflow_check_mul(a, b) \
	do { \
		if ((b) && (a) > (UT32_MAX / (b))) { \
			return -1; \
		} \
	} while (0)

#define overflow_check_add(a, b) \
	do { \
		if ((a) > (UT32_MAX - (b))) { \
			return -1; \
		} \
	} while (0)

/* Translate count to integer, consuming tokens in the process.
   Conversion terminates on the first non-digit character.

   Trying to consume something that isn't a count results in no
   consumption of input and a return of -1.

   Overflow consumes the rest of the digits, and returns -1.  */

static int
	consume_count(const char **type)
{
	// Note by RizinOrg:
	// to prevent the overflow check to be optimized out
	// by the compiler, this variable needs to be volatile.
	uint32_t count = 0;

	if (!isdigit((unsigned char)**type))
		return -1;

	while (isdigit((unsigned char)**type)) {
		overflow_check_mul(count, 10);
		count *= 10;

		uint32_t num = **type - '0';
		overflow_check_add(count, num);
		count += num;

		(*type)++;
	}

	return (count);
}

/* Like consume_count, but for counts that are preceded and followed
   by '_' if they are greater than 10.  Also, -1 is returned for
   failure, since 0 can be a valid value.  */

static int
	consume_count_with_underscores(const char **mangled)
{
	int idx;

	if (**mangled == '_') {
		(*mangled)++;
		if (!isdigit((unsigned char)**mangled))
			return -1;

		idx = consume_count(mangled);
		if (**mangled != '_')
			/* The trailing underscore was missing. */
			return -1;

		(*mangled)++;
	} else {
		if (**mangled < '0' || **mangled > '9')
			return -1;

		idx = **mangled - '0';
		(*mangled)++;
	}

	return idx;
}

/* C is the code for a type-qualifier.  Return the TYPE_QUAL
   corresponding to this qualifier.  */

static int
code_for_qualifier(int c)
{
	switch (c) {
	case 'C':
		return TYPE_QUAL_CONST;

	case 'V':
		return TYPE_QUAL_VOLATILE;

	case 'u':
		return TYPE_QUAL_RESTRICT;

	default:
		return TYPE_UNQUALIFIED;
	}
}

/* Return the string corresponding to the qualifiers given by
   TYPE_QUALS.  */

static const char *
qualifier_string(int type_quals)
{
	switch (type_quals) {
	case TYPE_UNQUALIFIED:
		return "";

	case TYPE_QUAL_CONST:
		return "const";

	case TYPE_QUAL_VOLATILE:
		return "volatile";

	case TYPE_QUAL_RESTRICT:
		return "__restrict";

	case TYPE_QUAL_CONST | TYPE_QUAL_VOLATILE:
		return "const volatile";

	case TYPE_QUAL_CONST | TYPE_QUAL_RESTRICT:
		return "const __restrict";

	case TYPE_QUAL_VOLATILE | TYPE_QUAL_RESTRICT:
		return "volatile __restrict";

	case TYPE_QUAL_CONST | TYPE_QUAL_VOLATILE | TYPE_QUAL_RESTRICT:
		return "const volatile __restrict";

	default:
		return "";
	}
}

/* C is the code for a type-qualifier.  Return the string
   corresponding to this qualifier.  This function should only be
   called with a valid qualifier code.  */

static const char *
demangle_qualifier(int c)
{
	return qualifier_string(code_for_qualifier(c));
}

/* char *cplus_demangle_v2 (const char *mangled, int options)

   If MANGLED is a mangled function name produced by GNU C++, then
   a pointer to a malloced string giving a C++ representation
   of the name will be returned; otherwise NULL will be returned.
   It is the caller's responsibility to free the string which
   is returned.

   The OPTIONS arg may contain one or more of the following bits:

	DMGL_ANSI	ANSI qualifiers such as `const' and `void' are
			included.
	DMGL_PARAMS	Function parameters are included.

   For example,

   cplus_demangle_v2 ("foo__1Ai", DMGL_PARAMS)		=> "A::foo(int)"
   cplus_demangle_v2 ("foo__1Ai", DMGL_PARAMS | DMGL_ANSI)	=> "A::foo(int)"
   cplus_demangle_v2 ("foo__1Ai", 0)			=> "A::foo"

   cplus_demangle_v2 ("foo__1Afe", DMGL_PARAMS)		=> "A::foo(float,...)"
   cplus_demangle_v2 ("foo__1Afe", DMGL_PARAMS | DMGL_ANSI)=> "A::foo(float,...)"
   cplus_demangle_v2 ("foo__1Afe", 0)			=> "A::foo"

   Note that any leading underscores, or other such characters prepended by
   the compilation system, are presumed to have already been stripped from
   MANGLED.  */

char *
	cplus_demangle_v2(const char *mangled, int options)
{
	char *ret;
	struct work_stuff work[1];
	memset((char *)work, 0, sizeof(work));
	work->options = options;
	if ((work->options & DMGL_STYLE_MASK) == 0)
		work->options |= (int)current_demangling_style & DMGL_STYLE_MASK;

	ret = internal_cplus_demangle(work, mangled);
	squangle_mop_up(work);
	return (ret);
}

/* This function performs most of what cplus_demangle_v2 use to do, but
   to be able to demangle a name with a B, K or n code, we need to
   have a longer term memory of what types have been seen. The original
   now intializes and cleans up the squangle code info, while internal
   calls go directly to this routine to avoid resetting that info. */

static char *
internal_cplus_demangle(struct work_stuff *work, const char *mangled)
{

	string decl;
	int success = 0;
	char *demangled = NULL;
	int s1, s2, s3, s4;
	s1 = work->constructor;
	s2 = work->destructor;
	s3 = work->static_type;
	s4 = work->type_quals;
	work->constructor = work->destructor = 0;
	work->type_quals = TYPE_UNQUALIFIED;
	work->dllimported = 0;

	if ((mangled != NULL) && (*mangled != '\0')) {
		string_init(&decl);

		/* First check to see if gnu style demangling is active and if the
		   string to be demangled contains a CPLUS_MARKER.  If so, attempt to
		   recognize one of the gnu special forms rather than looking for a
		   standard prefix.  In particular, don't worry about whether there
		   is a "__" string in the mangled string.  Consider "_$_5__foo" for
		   example.  */

		if ((AUTO_DEMANGLING || GNU_DEMANGLING)) {
			success = gnu_special(work, &mangled, &decl);
		}
		if (!success) {
			success = demangle_prefix(work, &mangled, &decl);
		}
		if (success && (*mangled != '\0')) {
			success = demangle_signature(work, &mangled, &decl);
		}
		if (work->constructor == 2) {
			string_prepend(&decl, "global constructors keyed to ");
			work->constructor = 0;
		} else if (work->destructor == 2) {
			string_prepend(&decl, "global destructors keyed to ");
			work->destructor = 0;
		} else if (work->dllimported == 1) {
			string_prepend(&decl, "import stub for ");
			work->dllimported = 0;
		}
		demangled = mop_up(work, &decl, success);
	}
	work->constructor = s1;
	work->destructor = s2;
	work->static_type = s3;
	work->type_quals = s4;
	return (demangled);
}

/* Clear out and squangling related storage */
static void
	squangle_mop_up(struct work_stuff *work)
{
	/* clean up the B and K type mangling types. */
	forget_B_and_K_types(work);
	if (work->btypevec != NULL) {
		free((char *)work->btypevec);
	}
	if (work->ktypevec != NULL) {
		free((char *)work->ktypevec);
	}
}

/* Clear out any mangled storage */

static char *
mop_up(struct work_stuff *work, string *declp, int success)
{
	char *demangled = NULL;

	/* Discard the remembered types, if any.  */

	forget_types(work);
	if (work->typevec != NULL) {
		free((char *)work->typevec);
		work->typevec = NULL;
		work->typevec_size = 0;
	}
	if (work->tmpl_argvec) {
		int i;

		for (i = 0; i < work->ntmpl_args; i++)
			if (work->tmpl_argvec[i])
				free((char *)work->tmpl_argvec[i]);

		free((char *)work->tmpl_argvec);
		work->tmpl_argvec = NULL;
	}
	if (work->previous_argument) {
		string_delete(work->previous_argument);
		free((char *)work->previous_argument);
		work->previous_argument = NULL;
	}

	/* If demangling was successful, ensure that the demangled string is null
	   terminated and return it.  Otherwise, free the demangling decl.  */

	if (!success) {
		string_delete(declp);
	} else {
		string_appendn(declp, "", 1);
		demangled = declp->b;
	}
	return (demangled);
}

/*

LOCAL FUNCTION

	demangle_signature -- demangle the signature part of a mangled name

SYNOPSIS

	static int
	demangle_signature (struct work_stuff *work, const char **mangled,
			    string *declp);

DESCRIPTION

	Consume and demangle the signature portion of the mangled name.

	DECLP is the string where demangled output is being built.  At
	entry it contains the demangled root name from the mangled name
	prefix.  I.E. either a demangled operator name or the root function
	name.  In some special cases, it may contain nothing.

	*MANGLED points to the current unconsumed location in the mangled
	name.  As tokens are consumed and demangling is performed, the
	pointer is updated to continuously point at the next token to
	be consumed.

	Demangling GNU style mangled names is nasty because there is no
	explicit token that marks the start of the outermost function
	argument list.  */

static int
demangle_signature(struct work_stuff *work, const char **mangled, string *declp)
{
	int success = 1;
	int func_done = 0;
	int expect_func = 0;
	int expect_return_type = 0;
	const char *oldmangled = NULL;
	string trawname;
	string tname;

	while (success && (**mangled != '\0')) {
		switch (**mangled) {
		case 'Q':
			oldmangled = *mangled;
			success = demangle_qualified(work, mangled, declp, 1, 0);
			if (success)
				remember_type(work, oldmangled, *mangled - oldmangled);
			if (AUTO_DEMANGLING || GNU_DEMANGLING)
				expect_func = 1;
			oldmangled = NULL;
			break;

		case 'K':
			oldmangled = *mangled;
			success = demangle_qualified(work, mangled, declp, 1, 0);
			if (AUTO_DEMANGLING || GNU_DEMANGLING) {
				expect_func = 1;
			}
			oldmangled = NULL;
			break;

		case 'S':
			/* Static member function */
			if (oldmangled == NULL) {
				oldmangled = *mangled;
			}
			(*mangled)++;
			work->static_type = 1;
			break;

		case 'C':
		case 'V':
		case 'u':
			work->type_quals |= code_for_qualifier(**mangled);

			/* a qualified member function */
			if (oldmangled == NULL)
				oldmangled = *mangled;
			(*mangled)++;
			break;

		case 'L':
			/* Local class name follows after "Lnnn_" */
			if (HP_DEMANGLING) {
				while (**mangled && (**mangled != '_'))
					(*mangled)++;
				if (!**mangled)
					success = 0;
				else
					(*mangled)++;
			} else
				success = 0;
			break;

		case '0':
		case '1':
		case '2':
		case '3':
		case '4':
		case '5':
		case '6':
		case '7':
		case '8':
		case '9':
			if (oldmangled == NULL) {
				oldmangled = *mangled;
			}
			work->temp_start = -1; /* uppermost call to demangle_class */
			success = demangle_class(work, mangled, declp);
			if (success) {
				remember_type(work, oldmangled, *mangled - oldmangled);
			}
			if (AUTO_DEMANGLING || GNU_DEMANGLING || EDG_DEMANGLING) {
				/* EDG and others will have the "F", so we let the loop cycle
				   if we are looking at one. */
				if (**mangled != 'F')
					expect_func = 1;
			}
			oldmangled = NULL;
			break;

		case 'B': {
			string s;
			success = do_type(work, mangled, &s);
			if (success) {
				string_append(&s, SCOPE_STRING(work));
				string_prepends(declp, &s);
			}
			oldmangled = NULL;
			expect_func = 1;
		} break;

		case 'F':
			/* Function */
			/* ARM/HP style demangling includes a specific 'F' character after
			   the class name.  For GNU style, it is just implied.  So we can
			   safely just consume any 'F' at this point and be compatible
			   with either style.  */

			oldmangled = NULL;
			func_done = 1;
			(*mangled)++;

			/* For lucid/ARM/HP style we have to forget any types we might
			   have remembered up to this point, since they were not argument
			   types.  GNU style considers all types seen as available for
			   back references.  See comment in demangle_args() */

			if (LUCID_DEMANGLING || ARM_DEMANGLING || HP_DEMANGLING || EDG_DEMANGLING) {
				forget_types(work);
			}
			success = demangle_args(work, mangled, declp);
			/* After picking off the function args, we expect to either
			   find the function return type (preceded by an '_') or the
			   end of the string. */
			if (success && (AUTO_DEMANGLING || EDG_DEMANGLING) && **mangled == '_') {
				++(*mangled);
				/* At this level, we do not care about the return type. */
				success = do_type(work, mangled, &tname);
				string_delete(&tname);
			}

			break;

		case 't':
			/* G++ Template */
			string_init(&trawname);
			string_init(&tname);
			if (oldmangled == NULL) {
				oldmangled = *mangled;
			}
			success = demangle_template(work, mangled, &tname,
				&trawname, 1, 1);
			if (success) {
				remember_type(work, oldmangled, *mangled - oldmangled);
			}
			string_append(&tname, SCOPE_STRING(work));

			string_prepends(declp, &tname);
			if (work->destructor & 1) {
				string_prepend(&trawname, "~");
				string_appends(declp, &trawname);
				work->destructor -= 1;
			}
			if ((work->constructor & 1) || (work->destructor & 1)) {
				string_appends(declp, &trawname);
				work->constructor -= 1;
			}
			string_delete(&trawname);
			string_delete(&tname);
			oldmangled = NULL;
			expect_func = 1;
			break;

		case '_':
			if (GNU_DEMANGLING && expect_return_type) {
				/* Read the return type. */
				string return_type;
				string_init(&return_type);

				(*mangled)++;
				success = do_type(work, mangled, &return_type);
				APPEND_BLANK(&return_type);

				string_prepends(declp, &return_type);
				string_delete(&return_type);
				break;
			} else
				/* At the outermost level, we cannot have a return type specified,
				   so if we run into another '_' at this point we are dealing with
				   a mangled name that is either bogus, or has been mangled by
				   some algorithm we don't know how to deal with.  So just
				   reject the entire demangling.  */
				/* However, "_nnn" is an expected suffix for alternate entry point
				   numbered nnn for a function, with HP aCC, so skip over that
				   without reporting failure. pai/1997-09-04 */
				if (HP_DEMANGLING) {
					(*mangled)++;
					while (**mangled && isdigit((unsigned char)**mangled))
						(*mangled)++;
				} else
					success = 0;
			break;

		case 'H':
			if (GNU_DEMANGLING) {
				/* A G++ template function.  Read the template arguments. */
				success = demangle_template(work, mangled, declp, 0, 0,
					0);
				if (!(work->constructor & 1))
					expect_return_type = 1;
				(*mangled)++;
				break;
			} else
			/* fall through */
			{
				;
			}

		default:
			if (AUTO_DEMANGLING || GNU_DEMANGLING) {
				/* Assume we have stumbled onto the first outermost function
				   argument token, and start processing args.  */
				func_done = 1;
				success = demangle_args(work, mangled, declp);
			} else {
				/* Non-GNU demanglers use a specific token to mark the start
				   of the outermost function argument tokens.  Typically 'F',
				   for ARM/HP-demangling, for example.  So if we find something
				   we are not prepared for, it must be an error.  */
				success = 0;
			}
			break;
		}
		/*
		  if (AUTO_DEMANGLING || GNU_DEMANGLING)
		  */
		{
			if (success && expect_func) {
				func_done = 1;
				if (LUCID_DEMANGLING || ARM_DEMANGLING || EDG_DEMANGLING) {
					forget_types(work);
				}
				success = demangle_args(work, mangled, declp);
				/* Since template include the mangling of their return types,
				   we must set expect_func to 0 so that we don't try do
				   demangle more arguments the next time we get here.  */
				expect_func = 0;
			}
		}
	}
	if (success && !func_done) {
		if (AUTO_DEMANGLING || GNU_DEMANGLING) {
			/* With GNU style demangling, bar__3foo is 'foo::bar(void)', and
			   bar__3fooi is 'foo::bar(int)'.  We get here when we find the
			   first case, and need to ensure that the '(void)' gets added to
			   the current declp.  Note that with ARM/HP, the first case
			   represents the name of a static data member 'foo::bar',
			   which is in the current declp, so we leave it alone.  */
			success = demangle_args(work, mangled, declp);
		}
	}
	if (success && PRINT_ARG_TYPES) {
		if (work->static_type)
			string_append(declp, " static");
		if (work->type_quals != TYPE_UNQUALIFIED) {
			APPEND_BLANK(declp);
			string_append(declp, qualifier_string(work->type_quals));
		}
	}

	return (success);
}

#if 0

static int
demangle_method_args (struct work_stuff *work, const char **mangled, string *declp)
{
  int success = 0;

  if (work -> static_type)
    {
      string_append (declp, *mangled + 1);
      *mangled += strlen (*mangled);
      success = 1;
    }
  else
    {
      success = demangle_args (work, mangled, declp);
    }
  return (success);
}

#endif

static int
demangle_template_template_parm(struct work_stuff *work, const char **mangled, string *tname)
{
	int i;
	int r;
	int need_comma = 0;
	int success = 1;
	string temp;

	string_append(tname, "template <");
	/* get size of template parameter list */
	if (get_count(mangled, &r)) {
		for (i = 0; i < r; i++) {
			if (need_comma) {
				string_append(tname, ", ");
			}

			/* Z for type parameters */
			if (**mangled == 'Z') {
				(*mangled)++;
				string_append(tname, "class");
			}
			/* z for template parameters */
			else if (**mangled == 'z') {
				(*mangled)++;
				success =
					demangle_template_template_parm(work, mangled, tname);
				if (!success) {
					break;
				}
			} else {
				/* temp is initialized in do_type */
				success = do_type(work, mangled, &temp);
				if (success) {
					string_appends(tname, &temp);
				}
				string_delete(&temp);
				if (!success) {
					break;
				}
			}
			need_comma = 1;
		}
	}
	// test cases failing because of this space
	// if (tname->p[-1] == '>')
	// 	string_append(tname, " ");
	string_append(tname, "> class");
	return (success);
}

static int
demangle_integral_value(struct work_stuff *work, const char **mangled, string *s)
{
	int success;

	if (**mangled == 'E') {
		int need_operator = 0;

		success = 1;
		string_appendn(s, "(", 1);
		(*mangled)++;
		while (success && **mangled != 'W' && **mangled != '\0') {
			if (need_operator) {
				size_t i;
				size_t len;

				success = 0;

				len = strlen(*mangled);

				for (i = 0;
					i < sizeof(optable) / sizeof(optable[0]);
					++i) {
					size_t l = strlen(optable[i].in);

					if (l <= len && memcmp(optable[i].in, *mangled, l) == 0) {
						string_appendn(s, " ", 1);
						string_append(s, optable[i].out);
						string_appendn(s, " ", 1);
						success = 1;
						(*mangled) += l;
						break;
					}
				}

				if (!success)
					break;
			} else
				need_operator = 1;

			success = demangle_template_value_parm(work, mangled, s,
				tk_integral);
		}

		if (**mangled != 'W')
			success = 0;
		else {
			string_appendn(s, ")", 1);
			(*mangled)++;
		}
	} else if (**mangled == 'Q' || **mangled == 'K')
		success = demangle_qualified(work, mangled, s, 0, 1);
	else {
		success = 0;

		if (**mangled == 'm') {
			string_appendn(s, "-", 1);
			(*mangled)++;
		}
		while (isdigit((unsigned char)**mangled)) {
			string_appendn(s, *mangled, 1);
			(*mangled)++;
			success = 1;
		}
	}

	return success;
}

static int
demangle_template_value_parm(struct work_stuff *work, const char **mangled, string *s, type_kind_t tk)
{
	int success = 1;

	if (**mangled == 'Y') {
		/* The next argument is a template parameter. */
		int idx;

		(*mangled)++;
		idx = consume_count_with_underscores(mangled);
		if (idx == -1 || (work->tmpl_argvec && idx >= work->ntmpl_args) || consume_count_with_underscores(mangled) == -1)
			return -1;
		if (work->tmpl_argvec)
			string_append(s, work->tmpl_argvec[idx]);
		else {
			char buf[10];
			snprintf(buf, sizeof(buf), "T%d", idx);
			string_append(s, buf);
		}
	} else if (tk == tk_integral)
		success = demangle_integral_value(work, mangled, s);
	else if (tk == tk_char) {
		char tmp[2];
		int val;
		if (**mangled == 'm') {
			string_appendn(s, "-", 1);
			(*mangled)++;
		}
		string_appendn(s, "'", 1);
		val = consume_count(mangled);
		if (val <= 0)
			success = 0;
		else {
			tmp[0] = (char)val;
			tmp[1] = '\0';
			string_appendn(s, &tmp[0], 1);
			string_appendn(s, "'", 1);
		}
	} else if (tk == tk_bool) {
		int val = consume_count(mangled);
		if (val == 0)
			string_appendn(s, "false", 5);
		else if (val == 1)
			string_appendn(s, "true", 4);
		else
			success = 0;
	} else if (tk == tk_real) {
		if (**mangled == 'm') {
			string_appendn(s, "-", 1);
			(*mangled)++;
		}
		while (isdigit((unsigned char)**mangled)) {
			string_appendn(s, *mangled, 1);
			(*mangled)++;
		}
		if (**mangled == '.') /* fraction */
		{
			string_appendn(s, ".", 1);
			(*mangled)++;
			while (isdigit((unsigned char)**mangled)) {
				string_appendn(s, *mangled, 1);
				(*mangled)++;
			}
		}
		if (**mangled == 'e') /* exponent */
		{
			string_appendn(s, "e", 1);
			(*mangled)++;
			while (isdigit((unsigned char)**mangled)) {
				string_appendn(s, *mangled, 1);
				(*mangled)++;
			}
		}
	} else if (tk == tk_pointer || tk == tk_reference) {
		if (**mangled == 'Q')
			success = demangle_qualified(work, mangled, s,
				/*isfuncname=*/0,
				/*append=*/1);
		else {
			int symbol_len = consume_count(mangled);
			if (symbol_len == -1)
				return -1;
			if (symbol_len == 0)
				string_appendn(s, "0", 1);
			else {
				char *p = malloc(symbol_len + 1), *q;
				strncpy(p, *mangled, symbol_len);
				p[symbol_len] = '\0';
				/* We use cplus_demangle_v2 here, rather than
				   internal_cplus_demangle, because the name of the entity
				   mangled here does not make use of any of the squangling
				   or type-code information we have built up thus far; it is
				   mangled independently.  */
				q = cplus_demangle_v2(p, work->options);
				if (tk == tk_pointer)
					string_appendn(s, "&", 1);
				/* FIXME: Pointer-to-member constants should get a
				   qualifying class name here.  */
				if (q) {
					string_append(s, q);
					free(q);
				} else
					string_append(s, p);
				free(p);
			}
			*mangled += symbol_len;
		}
	}

	return success;
}

/* Demangle the template name in MANGLED.  The full name of the
   template (e.g., S<int>) is placed in TNAME.  The name without the
   template parameters (e.g. S) is placed in TRAWNAME if TRAWNAME is
   non-NULL.  If IS_TYPE is nonzero, this template is a type template,
   not a function template.  If both IS_TYPE and REMEMBER are nonzero,
   the tmeplate is remembered in the list of back-referenceable
   types.  */

static int
demangle_template(struct work_stuff *work, const char **mangled, string *tname, string *trawname, int is_type, int remember)
{
	int i = 0;
	int r = 0;
	int need_comma = 0;
	int success = 0;
	int is_java_array = 0;
	string temp = { 0 };
	int bindex = 0;

	(*mangled)++;
	if (is_type) {
		if (remember)
			bindex = register_Btype(work);
		/* get template name */
		if (**mangled == 'z') {
			int idx;
			(*mangled)++;
			(*mangled)++;

			idx = consume_count_with_underscores(mangled);
			if (idx == -1 || (work->tmpl_argvec && idx >= work->ntmpl_args) || consume_count_with_underscores(mangled) == -1)
				return (0);

			if (work->tmpl_argvec) {
				string_append(tname, work->tmpl_argvec[idx]);
				if (trawname)
					string_append(trawname, work->tmpl_argvec[idx]);
			} else {
				char buf[10];
				snprintf(buf, sizeof(buf), "T%d", idx);
				string_append(tname, buf);
				if (trawname)
					string_append(trawname, buf);
			}
		} else {
			if ((r = consume_count(mangled)) <= 0 || (int)strlen(*mangled) < r) {
				return (0);
			}
			is_java_array = (work->options & DMGL_JAVA) && strncmp(*mangled, "JArray1Z", 8) == 0;
			if (!is_java_array) {
				string_appendn(tname, *mangled, r);
			}
			if (trawname)
				string_appendn(trawname, *mangled, r);
			*mangled += r;
		}
	}
	if (!is_java_array)
		string_append(tname, "<");
	/* get size of template parameter list */
	if (!get_count(mangled, &r)) {
		return (0);
	}
	if (!is_type) {
		/* Create an array for saving the template argument values. */
		work->tmpl_argvec = (char **)malloc(r * sizeof(char *));
		work->ntmpl_args = r;
		for (i = 0; i < r; i++)
			work->tmpl_argvec[i] = 0;
	}
	for (i = 0; i < r; i++) {
		if (need_comma) {
			string_append(tname, ", ");
		}
		/* Z for type parameters */
		if (**mangled == 'Z') {
			(*mangled)++;
			/* temp is initialized in do_type */
			success = do_type(work, mangled, &temp);
			if (success) {
				string_appends(tname, &temp);

				if (!is_type) {
					/* Save the template argument. */
					int len = temp.p - temp.b;
					work->tmpl_argvec[i] = malloc(len + 1);
					memcpy(work->tmpl_argvec[i], temp.b, len);
					work->tmpl_argvec[i][len] = '\0';
				}
			}
			string_delete(&temp);
			if (!success) {
				break;
			}
		}
		/* z for template parameters */
		else if (**mangled == 'z') {
			int r2;
			(*mangled)++;
			success = demangle_template_template_parm(work, mangled, tname);

			if (success && (r2 = consume_count(mangled)) > 0 && (int)strlen(*mangled) >= r2) {
				string_append(tname, " ");
				string_appendn(tname, *mangled, r2);
				if (!is_type) {
					/* Save the template argument. */
					int len = r2;
					work->tmpl_argvec[i] = malloc(len + 1);
					memcpy(work->tmpl_argvec[i], *mangled, len);
					work->tmpl_argvec[i][len] = '\0';
				}
				*mangled += r2;
			}
			if (!success) {
				break;
			}
		} else {
			string param;
			string *s;

			/* otherwise, value parameter */

			/* temp is initialized in do_type */
			success = do_type(work, mangled, &temp);
			string_delete(&temp);
			if (!success)
				break;

			if (!is_type) {
				s = &param;
				string_init(s);
			} else
				s = tname;

			success = demangle_template_value_parm(work, mangled, s,
				(type_kind_t)success);

			if (!success) {
				if (!is_type)
					string_delete(s);
				success = 0;
				break;
			}

			if (!is_type) {
				int len = s->p - s->b;
				work->tmpl_argvec[i] = malloc(len + 1);
				memcpy(work->tmpl_argvec[i], s->b, len);
				work->tmpl_argvec[i][len] = '\0';

				string_appends(tname, s);
				string_delete(s);
			}
		}
		need_comma = 1;
	}
	if (is_java_array) {
		string_append(tname, "[]");
	} else {
		// Test cases failing because of this extra space
		// if (tname->p[-1] == '>')
		// 	string_append(tname, " ");
		string_append(tname, ">");
	}

	if (is_type && remember)
		remember_Btype(work, tname->b, LEN_STRING(tname), bindex);

	return (success);
}

static int
arm_pt(struct work_stuff *work, const char *mangled, int n, const char **anchor, const char **args)
{
	/* Check if ARM template with "__pt__" in it ("parameterized type") */
	/* Allow HP also here, because HP's cfront compiler follows ARM to some extent */
	if ((ARM_DEMANGLING || HP_DEMANGLING) && (*anchor = mystrstr(mangled, "__pt__"))) {
		int len;
		*args = *anchor + 6;
		len = consume_count(args);
		if (len == -1)
			return 0;
		if (*args + len == mangled + n && **args == '_') {
			++*args;
			return 1;
		}
	}
	if (AUTO_DEMANGLING || EDG_DEMANGLING) {
		if ((*anchor = mystrstr(mangled, "__tm__")) || (*anchor = mystrstr(mangled, "__ps__")) || (*anchor = mystrstr(mangled, "__pt__"))) {
			int len;
			*args = *anchor + 6;
			len = consume_count(args);
			if (len == -1)
				return 0;
			if (*args + len == mangled + n && **args == '_') {
				++*args;
				return 1;
			}
		} else if ((*anchor = mystrstr(mangled, "__S"))) {
			int len;
			*args = *anchor + 3;
			len = consume_count(args);
			if (len == -1)
				return 0;
			if (*args + len == mangled + n && **args == '_') {
				++*args;
				return 1;
			}
		}
	}

	return 0;
}

static void
	demangle_arm_hp_template(struct work_stuff *work, const char **mangled, int n, string *declp)
{
	const char *p;
	const char *args;
	const char *e = *mangled + n;
	string arg;

	/* Check for HP aCC template spec: classXt1t2 where t1, t2 are
	   template args */
	if (HP_DEMANGLING && ((*mangled)[n] == 'X')) {
		char *start_spec_args = NULL;

		/* First check for and omit template specialization pseudo-arguments,
		   such as in "Spec<#1,#1.*>" */
		start_spec_args = strchr(*mangled, '<');
		if (start_spec_args && (start_spec_args - *mangled < n))
			string_appendn(declp, *mangled, start_spec_args - *mangled);
		else
			string_appendn(declp, *mangled, n);
		(*mangled) += n + 1;
		string_init(&arg);
		if (work->temp_start == -1) /* non-recursive call */
			work->temp_start = declp->p - declp->b;
		string_append(declp, "<");
		while (1) {
			string_clear(&arg);
			switch (**mangled) {
			case 'T':
				/* 'T' signals a type parameter */
				(*mangled)++;
				if (!do_type(work, mangled, &arg))
					goto hpacc_template_args_done;
				break;

			case 'U':
			case 'S':
				/* 'U' or 'S' signals an integral value */
				if (!do_hpacc_template_const_value(work, mangled, &arg))
					goto hpacc_template_args_done;
				break;

			case 'A':
				/* 'A' signals a named constant expression (literal) */
				if (!do_hpacc_template_literal(work, mangled, &arg))
					goto hpacc_template_args_done;
				break;

			default:
				/* Today, 1997-09-03, we have only the above types
				   of template parameters */
				/* FIXME: maybe this should fail and return null */
				goto hpacc_template_args_done;
			}
			string_appends(declp, &arg);
			/* Check if we're at the end of template args.
			    0 if at end of static member of template class,
			    _ if done with template args for a function */
			if ((**mangled == '\000') || (**mangled == '_'))
				break;
			else
				string_append(declp, ",");
		}
	hpacc_template_args_done:
		string_append(declp, ">");
		string_delete(&arg);
		if (**mangled == '_')
			(*mangled)++;
		return;
	}
	/* ARM template? (Also handles HP cfront extensions) */
	else if (arm_pt(work, *mangled, n, &p, &args)) {
		string type_str;

		string_init(&arg);
		string_appendn(declp, *mangled, p - *mangled);
		if (work->temp_start == -1) /* non-recursive call */
			work->temp_start = declp->p - declp->b;
		string_append(declp, "<");
		/* should do error checking here */
		while (args < e) {
			string_clear(&arg);

			/* Check for type or literal here */
			switch (*args) {
				/* HP cfront extensions to ARM for template args */
				/* spec: Xt1Lv1 where t1 is a type, v1 is a literal value */
				/* FIXME: We handle only numeric literals for HP cfront */
			case 'X':
				/* A typed constant value follows */
				args++;
				if (!do_type(work, &args, &type_str))
					goto cfront_template_args_done;
				string_append(&arg, "(");
				string_appends(&arg, &type_str);
				string_append(&arg, ")");
				if (*args != 'L')
					goto cfront_template_args_done;
				args++;
				/* Now snarf a literal value following 'L' */
				if (!snarf_numeric_literal(&args, &arg))
					goto cfront_template_args_done;
				break;

			case 'L':
				/* Snarf a literal following 'L' */
				args++;
				if (!snarf_numeric_literal(&args, &arg))
					goto cfront_template_args_done;
				break;
			default:
				/* Not handling other HP cfront stuff */
				if (!do_type(work, &args, &arg))
					goto cfront_template_args_done;
			}
			string_appends(declp, &arg);
			string_append(declp, ",");
		}
	cfront_template_args_done:
		string_delete(&arg);
		if (args >= e)
			--declp->p; /* remove extra comma */
		string_append(declp, ">");
	} else if (n > 10 && strncmp(*mangled, "_GLOBAL_", 8) == 0 && (*mangled)[9] == 'N' && (*mangled)[8] == (*mangled)[10] && strchr(cplus_markers, (*mangled)[8])) {
		/* A member of the anonymous namespace.  */
		string_append(declp, "{anonymous}");
	} else {
		if (work->temp_start == -1) /* non-recursive call only */
			work->temp_start = 0; /* disable in recursive calls */
		string_appendn(declp, *mangled, n);
	}
	*mangled += n;
}

/* Extract a class name, possibly a template with arguments, from the
   mangled string; qualifiers, local class indicators, etc. have
   already been dealt with */

static int
demangle_class_name(struct work_stuff *work, const char **mangled, string *declp)
{
	int n;
	int success = 0;

	n = consume_count(mangled);
	if (n == -1)
		return 0;
	if ((int)strlen(*mangled) >= n) {
		demangle_arm_hp_template(work, mangled, n, declp);
		success = 1;
	}

	return (success);
}

/*

LOCAL FUNCTION

	demangle_class -- demangle a mangled class sequence

SYNOPSIS

	static int
	demangle_class (struct work_stuff *work, const char **mangled,
			strint *declp)

DESCRIPTION

	DECLP points to the buffer into which demangling is being done.

	*MANGLED points to the current token to be demangled.  On input,
	it points to a mangled class (I.E. "3foo", "13verylongclass", etc.)
	On exit, it points to the next token after the mangled class on
	success, or the first unconsumed token on failure.

	If the CONSTRUCTOR or DESTRUCTOR flags are set in WORK, then
	we are demangling a constructor or destructor.  In this case
	we prepend "class::class" or "class::~class" to DECLP.

	Otherwise, we prepend "class::" to the current DECLP.

	Reset the constructor/destructor flags once they have been
	"consumed".  This allows demangle_class to be called later during
	the same demangling, to do normal class demangling.

	Returns 1 if demangling is successful, 0 otherwise.

*/

static int
demangle_class(struct work_stuff *work, const char **mangled, string *declp)
{
	int success = 0;
	int btype;
	string class_name;
	char *save_class_name_end = 0;

	string_init(&class_name);
	btype = register_Btype(work);
	if (demangle_class_name(work, mangled, &class_name)) {
		save_class_name_end = class_name.p;
		if ((work->constructor & 1) || (work->destructor & 1)) {
			/* adjust so we don't include template args */
			if (work->temp_start && (work->temp_start != -1)) {
				class_name.p = class_name.b + work->temp_start;
			}
			string_prepends(declp, &class_name);
			if (work->destructor & 1) {
				string_prepend(declp, "~");
				work->destructor -= 1;
			} else {
				work->constructor -= 1;
			}
		}
		class_name.p = save_class_name_end;
		remember_Ktype(work, class_name.b, LEN_STRING(&class_name));
		remember_Btype(work, class_name.b, LEN_STRING(&class_name), btype);
		string_prepend(declp, SCOPE_STRING(work));
		string_prepends(declp, &class_name);
		success = 1;
	}
	string_delete(&class_name);
	return (success);
}

/*

LOCAL FUNCTION

	demangle_prefix -- consume the mangled name prefix and find signature

SYNOPSIS

	static int
	demangle_prefix (struct work_stuff *work, const char **mangled,
			 string *declp);

DESCRIPTION

	Consume and demangle the prefix of the mangled name.

	DECLP points to the string buffer into which demangled output is
	placed.  On entry, the buffer is empty.  On exit it contains
	the root function name, the demangled operator name, or in some
	special cases either nothing or the completely demangled result.

	MANGLED points to the current pointer into the mangled name.  As each
	token of the mangled name is consumed, it is updated.  Upon entry
	the current mangled name pointer points to the first character of
	the mangled name.  Upon exit, it should point to the first character
	of the signature if demangling was successful, or to the first
	unconsumed character if demangling of the prefix was unsuccessful.

	Returns 1 on success, 0 otherwise.
 */

static int
demangle_prefix(struct work_stuff *work, const char **mangled, string *declp)
{
	int success = 1;
	const char *scan;
	int i;

	if (strlen(*mangled) > 6 && (strncmp(*mangled, "_imp__", 6) == 0 || strncmp(*mangled, "__imp_", 6) == 0)) {
		/* it's a symbol imported from a PE dynamic library. Check for both
		   new style prefix _imp__ and legacy __imp_ used by older versions
		   of dlltool. */
		(*mangled) += 6;
		work->dllimported = 1;
	} else if (strlen(*mangled) >= 11 && strncmp(*mangled, "_GLOBAL_", 8) == 0) {
		char *marker = strchr(cplus_markers, (*mangled)[8]);
		if (marker != NULL && *marker == (*mangled)[10]) {
			if ((*mangled)[9] == 'D') {
				/* it's a GNU global destructor to be executed at program exit */
				(*mangled) += 11;
				work->destructor = 2;
				if (gnu_special(work, mangled, declp))
					return success;
			} else if ((*mangled)[9] == 'I') {
				/* it's a GNU global constructor to be executed at program init */
				(*mangled) += 11;
				work->constructor = 2;
				if (gnu_special(work, mangled, declp))
					return success;
			}
		}
	} else if ((ARM_DEMANGLING || HP_DEMANGLING || EDG_DEMANGLING) && strncmp(*mangled, "__std__", 7) == 0) {
		/* it's a ARM global destructor to be executed at program exit */
		(*mangled) += 7;
		work->destructor = 2;
	} else if ((ARM_DEMANGLING || HP_DEMANGLING || EDG_DEMANGLING) && strncmp(*mangled, "__sti__", 7) == 0) {
		/* it's a ARM global constructor to be executed at program initial */
		(*mangled) += 7;
		work->constructor = 2;
	}

	/*  This block of code is a reduction in strength time optimization
	    of:
	    scan = mystrstr (*mangled, "__"); */

	{
		scan = *mangled;

		do {
			scan = strchr(scan, '_');
		} while (scan != NULL && *++scan != '_');

		if (scan != NULL)
			--scan;
	}

	if (scan != NULL) {
		/* We found a sequence of two or more '_', ensure that we start at
		   the last pair in the sequence.  */
		i = strspn(scan, "_");
		if (i > 2) {
			scan += (i - 2);
		}
	}

	if (scan == NULL) {
		success = 0;
	} else if (work->static_type) {
		if (!isdigit((unsigned char)scan[0]) && (scan[0] != 't')) {
			success = 0;
		}
	} else if ((scan == *mangled) && (isdigit((unsigned char)scan[2]) || (scan[2] == 'Q') || (scan[2] == 't') || (scan[2] == 'K') || (scan[2] == 'H'))) {
		/* The ARM says nothing about the mangling of local variables.
		   But cfront mangles local variables by prepending __<nesting_level>
		   to them. As an extension to ARM demangling we handle this case.  */
		if ((LUCID_DEMANGLING || ARM_DEMANGLING || HP_DEMANGLING) && isdigit((unsigned char)scan[2])) {
			*mangled = scan + 2;
			consume_count(mangled);
			string_append(declp, *mangled);
			*mangled += strlen(*mangled);
			success = 1;
		} else {
			/* A GNU style constructor starts with __[0-9Qt].  But cfront uses
			   names like __Q2_3foo3bar for nested type names.  So don't accept
			   this style of constructor for cfront demangling.  A GNU
			   style member-template constructor starts with 'H'. */
			if (!(LUCID_DEMANGLING || ARM_DEMANGLING || HP_DEMANGLING || EDG_DEMANGLING))
				work->constructor += 1;
			*mangled = scan + 2;
		}
	} else if (ARM_DEMANGLING && scan[2] == 'p' && scan[3] == 't') {
		/* Cfront-style parameterized type.  Handled later as a signature. */
		success = 1;

		/* ARM template? */
		demangle_arm_hp_template(work, mangled, strlen(*mangled), declp);
	} else if (EDG_DEMANGLING && ((scan[2] == 't' && scan[3] == 'm') || (scan[2] == 'p' && scan[3] == 's') || (scan[2] == 'p' && scan[3] == 't'))) {
		/* EDG-style parameterized type.  Handled later as a signature. */
		success = 1;

		/* EDG template? */
		demangle_arm_hp_template(work, mangled, strlen(*mangled), declp);
	} else if ((scan == *mangled) && !isdigit((unsigned char)scan[2]) && (scan[2] != 't')) {
		/* Mangled name starts with "__".  Skip over any leading '_' characters,
		   then find the next "__" that separates the prefix from the signature.
		   */
		if (!(ARM_DEMANGLING || LUCID_DEMANGLING || HP_DEMANGLING || EDG_DEMANGLING) || (arm_special(mangled, declp) == 0)) {
			while (*scan == '_') {
				scan++;
			}
			if ((scan = mystrstr(scan, "__")) == NULL || (*(scan + 2) == '\0')) {
				/* No separator (I.E. "__not_mangled"), or empty signature
				   (I.E. "__not_mangled_either__") */
				success = 0;
			} else {
				const char *tmp;

				/* Look for the LAST occurrence of __, allowing names to
				   have the '__' sequence embedded in them. */
				if (!(ARM_DEMANGLING || HP_DEMANGLING)) {
					while ((tmp = mystrstr(scan + 2, "__")) != NULL)
						scan = tmp;
				}
				if (*(scan + 2) == '\0')
					success = 0;
				else
					demangle_function_name(work, mangled, declp, scan);
			}
		}
	} else if (*(scan + 2) != '\0') {
		/* Mangled name does not start with "__" but does have one somewhere
		   in there with non empty stuff after it.  Looks like a global
		   function name.  */
		demangle_function_name(work, mangled, declp, scan);
	} else {
		/* Doesn't look like a mangled name */
		success = 0;
	}

	if (!success && (work->constructor == 2 || work->destructor == 2)) {
		string_append(declp, *mangled);
		*mangled += strlen(*mangled);
		success = 1;
	}
	return (success);
}

/*

LOCAL FUNCTION

	gnu_special -- special handling of gnu mangled strings

SYNOPSIS

	static int
	gnu_special (struct work_stuff *work, const char **mangled,
		     string *declp);


DESCRIPTION

	Process some special GNU style mangling forms that don't fit
	the normal pattern.  For example:

		_$_3foo		(destructor for class foo)
		_vt$foo		(foo virtual table)
		_vt$foo$bar	(foo::bar virtual table)
		__vt_foo	(foo virtual table, new style with thunks)
		_3foo$varname	(static data member)
		_Q22rs2tu$vw	(static data member)
		__t6vector1Zii	(constructor with template)
		__thunk_4__$_7ostream (virtual function thunk)
 */

static int
gnu_special(struct work_stuff *work, const char **mangled, string *declp)
{
	int n;
	int success = 1;
	const char *p;

	if ((*mangled)[0] == '_' && strchr(cplus_markers, (*mangled)[1]) != NULL && (*mangled)[2] == '_') {
		/* Found a GNU style destructor, get past "_<CPLUS_MARKER>_" */
		(*mangled) += 3;
		work->destructor += 1;
	} else if ((*mangled)[0] == '_' && (((*mangled)[1] == '_' && (*mangled)[2] == 'v' && (*mangled)[3] == 't' && (*mangled)[4] == '_') || ((*mangled)[1] == 'v' && (*mangled)[2] == 't' && strchr(cplus_markers, (*mangled)[3]) != NULL))) {
		/* Found a GNU style virtual table, get past "_vt<CPLUS_MARKER>"
		   and create the decl.  Note that we consume the entire mangled
		   input string, which means that demangle_signature has no work
		   to do.  */
		if ((*mangled)[2] == 'v')
			(*mangled) += 5; /* New style, with thunks: "__vt_" */
		else
			(*mangled) += 4; /* Old style, no thunks: "_vt<CPLUS_MARKER>" */
		while (**mangled != '\0') {
			switch (**mangled) {
			case 'Q':
			case 'K':
				success = demangle_qualified(work, mangled, declp, 0, 1);
				break;
			case 't':
				success = demangle_template(work, mangled, declp, 0, 1,
					1);
				break;
			default:
				if (isdigit((unsigned char)*mangled[0])) {
					n = consume_count(mangled);
					/* We may be seeing a too-large size, or else a
					   ".<digits>" indicating a static local symbol.  In
					   any case, declare victory and move on; *don't* try
					   to use n to allocate.  */
					if (n > (int)strlen(*mangled)) {
						success = 1;
						break;
					}
				} else {
					n = strcspn(*mangled, cplus_markers);
				}
				string_appendn(declp, *mangled, n);
				(*mangled) += n;
			}

			p = strpbrk(*mangled, cplus_markers);
			if (success && ((p == NULL) || (p == *mangled))) {
				if (p != NULL) {
					string_append(declp, SCOPE_STRING(work));
					(*mangled)++;
				}
			} else {
				success = 0;
				break;
			}
		}
		if (success)
			string_append(declp, " virtual table");
	} else if ((*mangled)[0] == '_' && (strchr("0123456789Qt", (*mangled)[1]) != NULL) && (p = strpbrk(*mangled, cplus_markers)) != NULL) {
		/* static data member, "_3foo$varname" for example */
		(*mangled)++;
		switch (**mangled) {
		case 'Q':
		case 'K':
			success = demangle_qualified(work, mangled, declp, 0, 1);
			break;
		case 't':
			success = demangle_template(work, mangled, declp, 0, 1, 1);
			break;
		default:
			n = consume_count(mangled);
			if (n < 0 || n > strlen(*mangled)) {
				success = 0;
				break;
			}
			string_appendn(declp, *mangled, n);
			(*mangled) += n;
		}
		if (success && (p == *mangled)) {
			/* Consumed everything up to the cplus_marker, append the
			   variable name.  */
			(*mangled)++;
			string_append(declp, SCOPE_STRING(work));
			n = strlen(*mangled);
			string_appendn(declp, *mangled, n);
			(*mangled) += n;
		} else {
			success = 0;
		}
	} else if (strncmp(*mangled, "__thunk_", 8) == 0) {
		int delta;

		(*mangled) += 8;
		delta = consume_count(mangled);
		if (delta == -1)
			success = 0;
		else {
			char *method = internal_cplus_demangle(work, ++*mangled);

			if (method) {
				char buf[50];
				snprintf(buf, sizeof(buf), "virtual function thunk (delta:%d) for ", -delta);
				string_append(declp, buf);
				string_append(declp, method);
				free(method);
				n = strlen(*mangled);
				(*mangled) += n;
			} else {
				success = 0;
			}
		}
	} else if (strncmp(*mangled, "__t", 3) == 0 && ((*mangled)[3] == 'i' || (*mangled)[3] == 'f')) {
		p = (*mangled)[3] == 'i' ? " type_info node" : " type_info function";
		(*mangled) += 4;
		switch (**mangled) {
		case 'Q':
		case 'K':
			success = demangle_qualified(work, mangled, declp, 0, 1);
			break;
		case 't':
			success = demangle_template(work, mangled, declp, 0, 1, 1);
			break;
		default:
			success = demangle_fund_type(work, mangled, declp);
			break;
		}
		if (success && **mangled != '\0')
			success = 0;
		if (success)
			string_append(declp, p);
	} else {
		success = 0;
	}
	return (success);
}

static void
	recursively_demangle(struct work_stuff *work, const char **mangled, string *result, int namelength)
{
	char *recurse = (char *)NULL;
	char *recurse_dem = (char *)NULL;

	recurse = (char *)malloc(namelength + 1);
	memcpy(recurse, *mangled, namelength);
	recurse[namelength] = '\000';

	recurse_dem = cplus_demangle_v2(recurse, work->options);

	if (recurse_dem) {
		string_append(result, recurse_dem);
		free(recurse_dem);
	} else {
		string_appendn(result, *mangled, namelength);
	}
	free(recurse);
	*mangled += namelength;
}

/*

LOCAL FUNCTION

	arm_special -- special handling of ARM/lucid mangled strings

SYNOPSIS

	static int
	arm_special (const char **mangled,
		     string *declp);


DESCRIPTION

	Process some special ARM style mangling forms that don't fit
	the normal pattern.  For example:

		__vtbl__3foo		(foo virtual table)
		__vtbl__3foo__3bar	(bar::foo virtual table)

 */

static int
	arm_special(const char **mangled, string *declp)
{
	int n;
	int success = 1;
	const char *scan;

	if (strncmp(*mangled, ARM_VTABLE_STRING, ARM_VTABLE_STRLEN) == 0) {
		/* Found a ARM style virtual table, get past ARM_VTABLE_STRING
		   and create the decl.  Note that we consume the entire mangled
		   input string, which means that demangle_signature has no work
		   to do.  */
		scan = *mangled + ARM_VTABLE_STRLEN;
		while (*scan != '\0') /* first check it can be demangled */
		{
			n = consume_count(&scan);
			if (n == -1) {
				return (0); /* no good */
			}
			scan += n;
			if (scan[0] == '_' && scan[1] == '_') {
				scan += 2;
			}
		}
		(*mangled) += ARM_VTABLE_STRLEN;
		while (**mangled != '\0') {
			n = consume_count(mangled);
			if (n == -1 || n > strlen(*mangled))
				return 0;
			string_prependn(declp, *mangled, n);
			(*mangled) += n;
			if ((*mangled)[0] == '_' && (*mangled)[1] == '_') {
				string_prepend(declp, "::");
				(*mangled) += 2;
			}
		}
		string_append(declp, " virtual table");
	} else {
		success = 0;
	}
	return (success);
}

/*

LOCAL FUNCTION

	demangle_qualified -- demangle 'Q' qualified name strings

SYNOPSIS

	static int
	demangle_qualified (struct work_stuff *, const char *mangled,
			    string *result, int isfuncname, int append);

DESCRIPTION

	Demangle a qualified name, such as "Q25Outer5Inner" which is
	the mangled form of "Outer::Inner".  The demangled output is
	prepended or appended to the result string according to the
	state of the append flag.

	If isfuncname is nonzero, then the qualified name we are building
	is going to be used as a member function name, so if it is a
	constructor or destructor function, append an appropriate
	constructor or destructor name.  I.E. for the above example,
	the result for use as a constructor is "Outer::Inner::Inner"
	and the result for use as a destructor is "Outer::Inner::~Inner".

BUGS

	Numeric conversion is ASCII dependent (FIXME).

 */

static int
demangle_qualified(struct work_stuff *work, const char **mangled, string *result, int isfuncname, int append)
{
	int qualifiers = 0;
	int success = 1;
	const char *p;
	char num[2];
	string temp;
	string last_name;
	int bindex = register_Btype(work);

	/* We only make use of ISFUNCNAME if the entity is a constructor or
	   destructor.  */
	isfuncname = (isfuncname && ((work->constructor & 1) || (work->destructor & 1)));

	string_init(&temp);
	string_init(&last_name);

	if ((*mangled)[0] == 'K') {
		/* Squangling qualified name reuse */
		int idx;
		(*mangled)++;
		idx = consume_count_with_underscores(mangled);
		if (idx == -1 || idx >= work->numk)
			success = 0;
		else
			string_append(&temp, work->ktypevec[idx]);
	} else
		switch ((*mangled)[1]) {
		case '_':
			/* GNU mangled name with more than 9 classes.  The count is preceded
			   by an underscore (to distinguish it from the <= 9 case) and followed
			   by an underscore.  */
			p = *mangled + 2;
			qualifiers = atoi(p);
			if (!isdigit((unsigned char)*p) || *p == '0')
				success = 0;

			/* Skip the digits.  */
			while (isdigit((unsigned char)*p))
				++p;

			if (*p != '_')
				success = 0;

			*mangled = p + 1;
			break;

		case '1':
		case '2':
		case '3':
		case '4':
		case '5':
		case '6':
		case '7':
		case '8':
		case '9':
			/* The count is in a single digit.  */
			num[0] = (*mangled)[1];
			num[1] = '\0';
			qualifiers = atoi(num);

			/* If there is an underscore after the digit, skip it.  This is
			   said to be for ARM-qualified names, but the ARM makes no
			   mention of such an underscore.  Perhaps cfront uses one.  */
			if ((*mangled)[2] == '_') {
				(*mangled)++;
			}
			(*mangled) += 2;
			break;

		case '0':
		default:
			success = 0;
		}

	if (!success)
		return success;

	/* Pick off the names and collect them in the temp buffer in the order
	   in which they are found, separated by '::'.  */

	while (qualifiers-- > 0) {
		int remember_K = 1;
		string_clear(&last_name);

		if (*mangled[0] == '_')
			(*mangled)++;

		if (*mangled[0] == 't') {
			/* Here we always append to TEMP since we will want to use
			   the template name without the template parameters as a
			   constructor or destructor name.  The appropriate
			   (parameter-less) value is returned by demangle_template
			   in LAST_NAME.  We do not remember the template type here,
			   in order to match the G++ mangling algorithm.  */
			success = demangle_template(work, mangled, &temp,
				&last_name, 1, 0);
			if (!success)
				break;
		} else if (*mangled[0] == 'K') {
			int idx;
			(*mangled)++;
			idx = consume_count_with_underscores(mangled);
			if (idx == -1 || idx >= work->numk)
				success = 0;
			else
				string_append(&temp, work->ktypevec[idx]);
			remember_K = 0;

			if (!success)
				break;
		} else {
			if (EDG_DEMANGLING) {
				int namelength;
				/* Now recursively demangle the qualifier
				 * This is necessary to deal with templates in
				 * mangling styles like EDG */
				namelength = consume_count(mangled);
				if (namelength == -1) {
					success = 0;
					break;
				}
				recursively_demangle(work, mangled, &temp, namelength);
			} else {
				success = do_type(work, mangled, &last_name);
				if (!success)
					break;
				string_appends(&temp, &last_name);
			}
		}

		if (remember_K)
			remember_Ktype(work, temp.b, LEN_STRING(&temp));

		if (qualifiers > 0)
			string_append(&temp, SCOPE_STRING(work));
	}

	remember_Btype(work, temp.b, LEN_STRING(&temp), bindex);

	/* If we are using the result as a function name, we need to append
	   the appropriate '::' separated constructor or destructor name.
	   We do this here because this is the most convenient place, where
	   we already have a pointer to the name and the length of the name.  */

	if (isfuncname) {
		string_append(&temp, SCOPE_STRING(work));
		if (work->destructor & 1)
			string_append(&temp, "~");
		string_appends(&temp, &last_name);
	}

	/* Now either prepend the temp buffer to the result, or append it,
	   depending upon the state of the append flag.  */

	if (append)
		string_appends(result, &temp);
	else {
		if (!STRING_EMPTY(result))
			string_append(&temp, SCOPE_STRING(work));
		string_prepends(result, &temp);
	}

	string_delete(&last_name);
	string_delete(&temp);
	return (success);
}

/*

LOCAL FUNCTION

	get_count -- convert an ascii count to integer, consuming tokens

SYNOPSIS

	static int
	get_count (const char **type, int *count)

DESCRIPTION

	Assume that *type points at a count in a mangled name; set
	*count to its value, and set *type to the next character after
	the count.  There are some weird rules in effect here.

	If *type does not point at a string of digits, return zero.

	If *type points at a string of digits followed by an
	underscore, set *count to their value as an integer, advance
	*type to point *after the underscore, and return 1.

	If *type points at a string of digits not followed by an
	underscore, consume only the first digit.  Set *count to its
	value as an integer, leave *type pointing after that digit,
	and return 1.

	The excuse for this odd behavior: in the ARM and HP demangling
	styles, a type can be followed by a repeat count of the form
	`Nxy', where:

	`x' is a single digit specifying how many additional copies
	    of the type to append to the argument list, and

	`y' is one or more digits, specifying the zero-based index of
	    the first repeated argument in the list.  Yes, as you're
	    unmangling the name you can figure this out yourself, but
	    it's there anyway.

	So, for example, in `bar__3fooFPiN51', the first argument is a
	pointer to an integer (`Pi'), and then the next five arguments
	are the same (`N5'), and the first repeat is the function's
	second argument (`1').
*/

static int
	get_count(const char **type, int *count)
{
	const char *p;
	int n;

	if (!isdigit((unsigned char)**type)) {
		return (0);
	} else {
		*count = **type - '0';
		(*type)++;
		if (isdigit((unsigned char)**type)) {
			p = *type;
			n = *count;
			do {
				n *= 10;
				n += *p - '0';
				p++;
			} while (isdigit((unsigned char)*p));
			if (*p == '_') {
				*type = p + 1;
				*count = n;
			}
		}
	}
	return (1);
}

/* RESULT will be initialised here; it will be freed on failure.  The
   value returned is really a type_kind_t.  */

static int
do_type(struct work_stuff *work, const char **mangled, string *result)
{
	int n;
	int done;
	int success;
	string decl;
	const char *remembered_type;
	int type_quals;
	string btype;
	type_kind_t tk = tk_none;

	string_init(&btype);
	string_init(&decl);
	string_init(result);

	done = 0;
	success = 1;
	while (success && !done) {
		int member;
		switch (**mangled) {

			/* A pointer type */
		case 'P':
		case 'p':
			(*mangled)++;
			if (!(work->options & DMGL_JAVA))
				string_prepend(&decl, "*");
			if (tk == tk_none)
				tk = tk_pointer;
			break;

			/* A reference type */
		case 'R':
			(*mangled)++;
			string_prepend(&decl, "&");
			if (tk == tk_none)
				tk = tk_reference;
			break;

			/* An array */
		case 'A': {
			++(*mangled);
			if (!STRING_EMPTY(&decl) && (decl.b[0] == '*' || decl.b[0] == '&')) {
				string_prepend(&decl, "(");
				string_append(&decl, ")");
			}
			string_append(&decl, "[");
			if (**mangled != '_')
				success = demangle_template_value_parm(work, mangled, &decl,
					tk_integral);
			if (**mangled == '_')
				++(*mangled);
			string_append(&decl, "]");
			break;
		}

		/* A back reference to a previously seen type */
		case 'T':
			(*mangled)++;
			if (!get_count(mangled, &n) || n >= work->ntypes) {
				success = 0;
			} else {
				remembered_type = work->typevec[n];
				mangled = &remembered_type;
			}
			break;

			/* A function */
		case 'F':
			(*mangled)++;
			if (!STRING_EMPTY(&decl) && (decl.b[0] == '*' || decl.b[0] == '&')) {
				string_prepend(&decl, "(");
				string_append(&decl, ")");
			}
			/* After picking off the function args, we expect to either find the
			   function return type (preceded by an '_') or the end of the
			   string.  */
			if (!demangle_nested_args(work, mangled, &decl) || (**mangled != '_' && **mangled != '\0')) {
				success = 0;
				break;
			}
			if (success && (**mangled == '_'))
				(*mangled)++;
			break;

		case 'O': {
			(*mangled)++;
			string_prepend(&decl, "&&");
			if (tk == tk_none)
				tk = tk_reference;
			break;
		}
		case 'M': {
			type_quals = TYPE_UNQUALIFIED;

			member = **mangled == 'M';
			(*mangled)++;

			string_append(&decl, ")");
			string_prepend(&decl, SCOPE_STRING(work));
			if (isdigit((unsigned char)**mangled)) {
				n = consume_count(mangled);
				if (n == -1 || (int)strlen(*mangled) < n) {
					success = 0;
					break;
				}
				string_prependn(&decl, *mangled, n);
				*mangled += n;
			} else if (**mangled == 'X' || **mangled == 'Y') {
				string temp;
				do_type(work, mangled, &temp);
				string_prepends(&decl, &temp);
			} else if (**mangled == 't') {
				string temp;
				string_init(&temp);
				success = demangle_template(work, mangled, &temp,
					NULL, 1, 1);
				if (success) {
					string_prependn(&decl, temp.b, temp.p - temp.b);
					string_clear(&temp);
				} else
					break;
			} else {
				success = 0;
				break;
			}

			string_prepend(&decl, "(");
			if (member) {
				switch (**mangled) {
				case 'C':
				case 'V':
				case 'u':
					type_quals |= code_for_qualifier(**mangled);
					(*mangled)++;
					break;

				default:
					break;
				}

				if (*(*mangled)++ != 'F') {
					success = 0;
					break;
				}
			}
			if ((member && !demangle_nested_args(work, mangled, &decl)) || **mangled != '_') {
				success = 0;
				break;
			}
			(*mangled)++;
			if (!PRINT_ANSI_QUALIFIERS) {
				break;
			}
			if (type_quals != TYPE_UNQUALIFIED) {
				APPEND_BLANK(&decl);
				string_append(&decl, qualifier_string(type_quals));
			}
			break;
		}
		case 'G':
			(*mangled)++;
			break;

		case 'C':
		case 'V':
		case 'u':
			if (PRINT_ANSI_QUALIFIERS) {
				if (!STRING_EMPTY(&decl))
					string_prepend(&decl, " ");

				string_prepend(&decl, demangle_qualifier(**mangled));
			}
			(*mangled)++;
			break;
			/*
			  }
			  */

			/* fall through */
		default:
			done = 1;
			break;
		}
	}

	if (success)
		switch (**mangled) {
			/* A qualified name, such as "Outer::Inner".  */
		case 'Q':
		case 'K': {
			success = demangle_qualified(work, mangled, result, 0, 1);
			break;
		}

		/* A back reference to a previously seen squangled type */
		case 'B':
			(*mangled)++;
			if (!get_count(mangled, &n) || n >= work->numb)
				success = 0;
			else
				string_append(result, work->btypevec[n]);
			break;

		case 'X':
		case 'Y':
			/* A template parm.  We substitute the corresponding argument. */
			{
				int idx;

				(*mangled)++;
				idx = consume_count_with_underscores(mangled);

				if (idx == -1 || (work->tmpl_argvec && idx >= work->ntmpl_args) || consume_count_with_underscores(mangled) == -1) {
					success = 0;
					break;
				}

				if (work->tmpl_argvec)
					string_append(result, work->tmpl_argvec[idx]);
				else {
					char buf[10];
					snprintf(buf, sizeof(buf), "T%d", idx);
					string_append(result, buf);
				}

				success = 1;
			}
			break;

		default:
			success = demangle_fund_type(work, mangled, result);
			if (tk == tk_none)
				tk = (type_kind_t)success;
			break;
		}

	if (success) {
		if (!STRING_EMPTY(&decl)) {
			string_append(result, " ");
			string_appends(result, &decl);
		}
	} else
		string_delete(result);
	string_delete(&decl);

	if (success)
		/* Assume an integral type, if we're not sure.  */
		return (int)((tk == tk_none) ? tk_integral : tk);
	else
		return 0;
}

/* Given a pointer to a type string that represents a fundamental type
   argument (int, long, unsigned int, etc) in TYPE, a pointer to the
   string in which the demangled output is being built in RESULT, and
   the WORK structure, decode the types and add them to the result.

   For example:

	"Ci"	=>	"const int"
	"Sl"	=>	"signed long"
	"CUs"	=>	"const unsigned short"

   The value returned is really a type_kind_t.  */

static int
demangle_fund_type(struct work_stuff *work, const char **mangled, string *result)
{
	int done = 0;
	int success = 1;
	char buf[10];
	int dec = 0;
	string btype;
	type_kind_t tk = tk_integral;

	string_init(&btype);

	/* First pick off any type qualifiers.  There can be more than one.  */

	while (!done) {
		switch (**mangled) {
		case 'C':
		case 'V':
		case 'u':
			if (PRINT_ANSI_QUALIFIERS) {
				if (!STRING_EMPTY(result))
					string_prepend(result, " ");
				string_prepend(result, demangle_qualifier(**mangled));
			}
			(*mangled)++;
			break;
		case 'U':
			(*mangled)++;
			APPEND_BLANK(result);
			string_append(result, "unsigned");
			break;
		case 'S': /* signed char only */
			(*mangled)++;
			APPEND_BLANK(result);
			string_append(result, "signed");
			break;
		case 'J':
			(*mangled)++;
			APPEND_BLANK(result);
			string_append(result, "__complex");
			break;
		default:
			done = 1;
			break;
		}
	}

	/* Now pick off the fundamental type.  There can be only one.  */

	switch (**mangled) {
	case '\0':
	case '_':
		break;
	case 'v':
		(*mangled)++;
		APPEND_BLANK(result);
		string_append(result, "void");
		break;
	case 'x':
		(*mangled)++;
		APPEND_BLANK(result);
		string_append(result, "long long");
		break;
	case 'l':
		(*mangled)++;
		APPEND_BLANK(result);
		string_append(result, "long");
		break;
	case 'i':
		(*mangled)++;
		APPEND_BLANK(result);
		string_append(result, "int");
		break;
	case 's':
		(*mangled)++;
		APPEND_BLANK(result);
		string_append(result, "short");
		break;
	case 'b':
		(*mangled)++;
		APPEND_BLANK(result);
		string_append(result, "bool");
		tk = tk_bool;
		break;
	case 'c':
		(*mangled)++;
		APPEND_BLANK(result);
		string_append(result, "char");
		tk = tk_char;
		break;
	case 'w':
		(*mangled)++;
		APPEND_BLANK(result);
		string_append(result, "wchar_t");
		tk = tk_char;
		break;
	case 'r':
		(*mangled)++;
		APPEND_BLANK(result);
		string_append(result, "long double");
		tk = tk_real;
		break;
	case 'd':
		(*mangled)++;
		APPEND_BLANK(result);
		string_append(result, "double");
		tk = tk_real;
		break;
	case 'f':
		(*mangled)++;
		APPEND_BLANK(result);
		string_append(result, "float");
		tk = tk_real;
		break;
	case 'G':
		(*mangled)++;
		if (!isdigit((unsigned char)**mangled)) {
			success = 0;
			break;
		}
	case 'I':
		(*mangled)++;
		if (**mangled == '_') {
			int i;
			(*mangled)++;
			for (i = 0;
				i < sizeof(buf) - 1 && **mangled && **mangled != '_';
				(*mangled)++, i++) {
				buf[i] = **mangled;
			}
			if (**mangled != '_') {
				success = 0;
				break;
			}
			buf[i] = '\0';
			(*mangled)++;
		} else {
			strncpy(buf, *mangled, 2);
			buf[2] = '\0';
			*mangled += min(strlen(*mangled), 2);
		}
		sscanf(buf, "%x", &dec);
		if (dec > 64 || dec < 8) {
			success = 0;
			break;
		}
		snprintf(buf, sizeof(buf), "int%i_t", dec);
		APPEND_BLANK(result);
		string_append(result, buf);
		break;

		/* fall through */
		/* An explicit type, such as "6mytype" or "7integer" */
	case '0':
	case '1':
	case '2':
	case '3':
	case '4':
	case '5':
	case '6':
	case '7':
	case '8':
	case '9': {
		int bindex = register_Btype(work);
		string btype;
		string_init(&btype);
		if (demangle_class_name(work, mangled, &btype)) {
			remember_Btype(work, btype.b, LEN_STRING(&btype), bindex);
			APPEND_BLANK(result);
			string_appends(result, &btype);
		} else
			success = 0;
		string_delete(&btype);
		break;
	}
	case 't': {
		success = demangle_template(work, mangled, &btype, 0, 1, 1);
		string_appends(result, &btype);
		break;
	}
	default:
		success = 0;
		break;
	}

	return success ? ((int)tk) : 0;
}

/* Handle a template's value parameter for HP aCC (extension from ARM)
 **mangled points to 'S' or 'U' */

static int
do_hpacc_template_const_value(struct work_stuff *work, const char **mangled, string *result)
{
	int unsigned_const;

	if (**mangled != 'U' && **mangled != 'S')
		return 0;

	unsigned_const = (**mangled == 'U');

	(*mangled)++;

	switch (**mangled) {
	case 'N':
		string_append(result, "-");
		/* fall through */
	case 'P':
		(*mangled)++;
		break;
	case 'M':
		/* special case for -2^31 */
		string_append(result, "-2147483648");
		(*mangled)++;
		return 1;
	default:
		return 0;
	}

	/* We have to be looking at an integer now */
	if (!(isdigit((unsigned char)**mangled)))
		return 0;

	/* We only deal with integral values for template
	   parameters -- so it's OK to look only for digits */
	while (isdigit((unsigned char)**mangled)) {
		char_str[0] = **mangled;
		string_append(result, char_str);
		(*mangled)++;
	}

	if (unsigned_const)
		string_append(result, "U");

	/* FIXME? Some day we may have 64-bit (or larger :-) ) constants
	   with L or LL suffixes. pai/1997-09-03 */

	return 1; /* success */
}

/* Handle a template's literal parameter for HP aCC (extension from ARM)
 **mangled is pointing to the 'A' */

static int
do_hpacc_template_literal(struct work_stuff *work, const char **mangled, string *result)
{
	int literal_len = 0;
	char *recurse;
	char *recurse_dem;

	if (**mangled != 'A')
		return 0;

	(*mangled)++;

	literal_len = consume_count(mangled);

	if (literal_len <= 0)
		return 0;

	/* Literal parameters are names of arrays, functions, etc.  and the
	   canonical representation uses the address operator */
	string_append(result, "&");

	/* Now recursively demangle the literal name */
	recurse = (char *)malloc(literal_len + 1);
	memcpy(recurse, *mangled, literal_len);
	recurse[literal_len] = '\000';

	recurse_dem = cplus_demangle_v2(recurse, work->options);

	if (recurse_dem) {
		string_append(result, recurse_dem);
		free(recurse_dem);
	} else {
		string_appendn(result, *mangled, literal_len);
	}
	(*mangled) += literal_len;
	free(recurse);

	return 1;
}

static int
	snarf_numeric_literal(const char **args, string *arg)
{
	if (**args == '-') {
		char_str[0] = '-';
		string_append(arg, char_str);
		(*args)++;
	} else if (**args == '+')
		(*args)++;

	if (!isdigit((unsigned char)**args))
		return 0;

	while (isdigit((unsigned char)**args)) {
		char_str[0] = **args;
		string_append(arg, char_str);
		(*args)++;
	}

	return 1;
}

/* Demangle the next argument, given by MANGLED into RESULT, which
   *should be an uninitialized* string.  It will be initialized here,
   and free'd should anything go wrong.  */

static int
do_arg(struct work_stuff *work, const char **mangled, string *result)
{
	/* Remember where we started so that we can record the type, for
	   non-squangling type remembering.  */
	const char *start = *mangled;

	string_init(result);

	if (work->nrepeats > 0) {
		--work->nrepeats;

		if (work->previous_argument == 0)
			return 0;

		/* We want to reissue the previous type in this argument list.  */
		string_appends(result, work->previous_argument);
		return 1;
	}

	if (**mangled == 'n') {
		/* A squangling-style repeat.  */
		(*mangled)++;
		work->nrepeats = consume_count(mangled);

		if (work->nrepeats <= 0)
			/* This was not a repeat count after all.  */
			return 0;

		if (work->nrepeats > 9) {
			if (**mangled != '_')
				/* The repeat count should be followed by an '_' in this
				   case.  */
				return 0;
			else
				(*mangled)++;
		}

		/* Now, the repeat is all set up.  */
		return do_arg(work, mangled, result);
	}

	/* Save the result in WORK->previous_argument so that we can find it
	   if it's repeated.  Note that saving START is not good enough: we
	   do not want to add additional types to the back-referenceable
	   type vector when processing a repeated type.  */
	if (work->previous_argument)
		string_clear(work->previous_argument);
	else {
		work->previous_argument = (string *)malloc(sizeof(string));
		string_init(work->previous_argument);
	}

	if (!do_type(work, mangled, work->previous_argument))
		return 0;

	string_appends(result, work->previous_argument);

	remember_type(work, start, *mangled - start);
	return 1;
}

static void
	remember_type(struct work_stuff *work, const char *start, int len)
{
	char *tem;

	if (work->forgetting_types)
		return;

	if (work->ntypes >= work->typevec_size) {
		if (work->typevec_size == 0) {
			work->typevec_size = 3;
			work->typevec = (char **)malloc(sizeof(char *) * work->typevec_size);
		} else {
			work->typevec_size *= 2;
			work->typevec = (char **)realloc((char *)work->typevec,
				sizeof(char *) * work->typevec_size);
		}
	}
	tem = malloc(len + 1);
	memcpy(tem, start, len);
	tem[len] = '\0';
	work->typevec[work->ntypes++] = tem;
}

/* Remember a K type class qualifier. */
static void
	remember_Ktype(struct work_stuff *work, const char *start, int len)
{
	char *tem;

	if (work->numk >= work->ksize) {
		if (work->ksize == 0) {
			work->ksize = 5;
			work->ktypevec = (char **)malloc(sizeof(char *) * work->ksize);
		} else {
			work->ksize *= 2;
			work->ktypevec = (char **)realloc((char *)work->ktypevec,
				sizeof(char *) * work->ksize);
		}
	}
	tem = malloc(len + 1);
	memcpy(tem, start, len);
	tem[len] = '\0';
	work->ktypevec[work->numk++] = tem;
}

/* Register a B code, and get an index for it. B codes are registered
   as they are seen, rather than as they are completed, so map<temp<char> >
   registers map<temp<char> > as B0, and temp<char> as B1 */

static int
register_Btype(struct work_stuff *work)
{
	int ret;

	if (work->numb >= work->bsize) {
		if (work->bsize == 0) {
			work->bsize = 5;
			work->btypevec = (char **)malloc(sizeof(char *) * work->bsize);
		} else {
			work->bsize *= 2;
			work->btypevec = (char **)realloc((char *)work->btypevec,
				sizeof(char *) * work->bsize);
		}
	}
	ret = work->numb++;
	work->btypevec[ret] = NULL;
	return (ret);
}

/* Store a value into a previously registered B code type. */

static void
	remember_Btype(struct work_stuff *work, const char *start, int len, int index)
{
	char *tem;

	tem = malloc(len + 1);
	memcpy(tem, start, len);
	tem[len] = '\0';
	work->btypevec[index] = tem;
}

/* Lose all the info related to B and K type codes. */
static void
	forget_B_and_K_types(struct work_stuff *work)
{
	int i;

	while (work->numk > 0) {
		i = --(work->numk);
		if (work->ktypevec[i] != NULL) {
			free(work->ktypevec[i]);
			work->ktypevec[i] = NULL;
		}
	}

	while (work->numb > 0) {
		i = --(work->numb);
		if (work->btypevec[i] != NULL) {
			free(work->btypevec[i]);
			work->btypevec[i] = NULL;
		}
	}
}
/* Forget the remembered types, but not the type vector itself.  */

static void
	forget_types(struct work_stuff *work)
{
	int i;

	while (work->ntypes > 0) {
		i = --(work->ntypes);
		if (work->typevec[i] != NULL) {
			free(work->typevec[i]);
			work->typevec[i] = NULL;
		}
	}
}

/* Process the argument list part of the signature, after any class spec
   has been consumed, as well as the first 'F' character (if any).  For
   example:

   "__als__3fooRT0"		=>	process "RT0"
   "complexfunc5__FPFPc_PFl_i"	=>	process "PFPc_PFl_i"

   DECLP must be already initialised, usually non-empty.  It won't be freed
   on failure.

   Note that g++ differs significantly from ARM and lucid style mangling
   with regards to references to previously seen types.  For example, given
   the source fragment:

     class foo {
       public:
       foo::foo (int, foo &ia, int, foo &ib, int, foo &ic);
     };

     foo::foo (int, foo &ia, int, foo &ib, int, foo &ic) { ia = ib = ic; }
     void foo (int, foo &ia, int, foo &ib, int, foo &ic) { ia = ib = ic; }

   g++ produces the names:

     __3fooiRT0iT2iT2
     foo__FiR3fooiT1iT1

   while lcc (and presumably other ARM style compilers as well) produces:

     foo__FiR3fooT1T2T1T2
     __ct__3fooFiR3fooT1T2T1T2

   Note that g++ bases its type numbers starting at zero and counts all
   previously seen types, while lucid/ARM bases its type numbers starting
   at one and only considers types after it has seen the 'F' character
   indicating the start of the function args.  For lucid/ARM style, we
   account for this difference by discarding any previously seen types when
   we see the 'F' character, and subtracting one from the type number
   reference.

 */

static int
demangle_args(struct work_stuff *work, const char **mangled, string *declp)
{
	string arg;
	int need_comma = 0;
	int r;
	int t;
	const char *tem;
	char temptype;

	if (PRINT_ARG_TYPES) {
		string_append(declp, "(");
		if (**mangled == '\0') {
			string_append(declp, "void");
		}
	}

	while ((**mangled != '_' && **mangled != '\0' && **mangled != 'e') || work->nrepeats > 0) {
		if ((**mangled == 'N') || (**mangled == 'T')) {
			temptype = *(*mangled)++;

			if (temptype == 'N') {
				if (!get_count(mangled, &r)) {
					return (0);
				}
			} else {
				r = 1;
			}
			if ((HP_DEMANGLING || ARM_DEMANGLING || EDG_DEMANGLING) && work->ntypes >= 10) {
				/* If we have 10 or more types we might have more than a 1 digit
				   index so we'll have to consume the whole count here. This
				   will lose if the next thing is a type name preceded by a
				   count but it's impossible to demangle that case properly
				   anyway. Eg if we already have 12 types is T12Pc "(..., type1,
				   Pc, ...)"  or "(..., type12, char *, ...)" */
				if ((t = consume_count(mangled)) <= 0) {
					return (0);
				}
			} else {
				if (!get_count(mangled, &t)) {
					return (0);
				}
			}
			if (LUCID_DEMANGLING || ARM_DEMANGLING || HP_DEMANGLING || EDG_DEMANGLING) {
				t--;
			}
			/* Validate the type index.  Protect against illegal indices from
			   malformed type strings.  */
			if ((t < 0) || (t >= work->ntypes)) {
				return (0);
			}
			while (work->nrepeats > 0 || --r >= 0) {
				tem = work->typevec[t];
				if (need_comma && PRINT_ARG_TYPES) {
					string_append(declp, ", ");
				}
				if (!do_arg(work, &tem, &arg)) {
					return (0);
				}
				if (PRINT_ARG_TYPES) {
					string_appends(declp, &arg);
				}
				string_delete(&arg);
				need_comma = 1;
			}
		} else {
			if (need_comma && PRINT_ARG_TYPES)
				string_append(declp, ", ");
			if (!do_arg(work, mangled, &arg))
				return (0);
			if (PRINT_ARG_TYPES)
				string_appends(declp, &arg);
			string_delete(&arg);
			need_comma = 1;
		}
	}

	if (**mangled == 'e') {
		(*mangled)++;
		if (PRINT_ARG_TYPES) {
			if (need_comma) {
				string_append(declp, ",");
			}
			string_append(declp, "...");
		}
	}

	if (PRINT_ARG_TYPES) {
		string_append(declp, ")");
	}
	return (1);
}

/* Like demangle_args, but for demangling the argument lists of function
   and method pointers or references, not top-level declarations.  */

static int
demangle_nested_args(struct work_stuff *work, const char **mangled, string *declp)
{
	string *saved_previous_argument;
	int result;
	int saved_nrepeats;

	/* The G++ name-mangling algorithm does not remember types on nested
	   argument lists, unless -fsquangling is used, and in that case the
	   type vector updated by remember_type is not used.  So, we turn
	   off remembering of types here.  */
	++work->forgetting_types;

	/* For the repeat codes used with -fsquangling, we must keep track of
	   the last argument.  */
	saved_previous_argument = work->previous_argument;
	saved_nrepeats = work->nrepeats;
	work->previous_argument = 0;
	work->nrepeats = 0;

	/* Actually demangle the arguments.  */
	result = demangle_args(work, mangled, declp);

	/* Restore the previous_argument field.  */
	if (work->previous_argument)
		string_delete(work->previous_argument);
	work->previous_argument = saved_previous_argument;
	--work->forgetting_types;
	work->nrepeats = saved_nrepeats;

	return result;
}

static void
	demangle_function_name(struct work_stuff *work, const char **mangled, string *declp, const char *scan)
{
	size_t i;
	string type;
	const char *tem;

	string_appendn(declp, (*mangled), scan - (*mangled));
	string_need(declp, 1);
	*(declp->p) = '\0';

	/* Consume the function name, including the "__" separating the name
	   from the signature.  We are guaranteed that SCAN points to the
	   separator.  */

	(*mangled) = scan + 2;
	/* We may be looking at an instantiation of a template function:
	   foo__Xt1t2_Ft3t4, where t1, t2, ... are template arguments and a
	   following _F marks the start of the function arguments.  Handle
	   the template arguments first. */

	if (HP_DEMANGLING && (**mangled == 'X')) {
		demangle_arm_hp_template(work, mangled, 0, declp);
		/* This leaves MANGLED pointing to the 'F' marking func args */
	}

	if (LUCID_DEMANGLING || ARM_DEMANGLING || HP_DEMANGLING || EDG_DEMANGLING) {

		/* See if we have an ARM style constructor or destructor operator.
		   If so, then just record it, clear the decl, and return.
		   We can't build the actual constructor/destructor decl until later,
		   when we recover the class name from the signature.  */

		if (strcmp(declp->b, "__ct") == 0) {
			work->constructor += 1;
			string_clear(declp);
			return;
		} else if (strcmp(declp->b, "__dt") == 0) {
			work->destructor += 1;
			string_clear(declp);
			return;
		}
	}

	if (declp->p - declp->b >= 3 && declp->b[0] == 'o' && declp->b[1] == 'p' && strchr(cplus_markers, declp->b[2]) != NULL) {
		/* see if it's an assignment expression */
		if (declp->p - declp->b >= 10 /* op$assign_ */
			&& memcmp(declp->b + 3, "assign_", 7) == 0) {
			for (i = 0; i < sizeof(optable) / sizeof(optable[0]); i++) {
				int len = declp->p - declp->b - 10;
				if ((int)strlen(optable[i].in) == len && memcmp(optable[i].in, declp->b + 10, len) == 0) {
					string_clear(declp);
					string_append(declp, "operator");
					string_append(declp, optable[i].out);
					string_append(declp, "=");
					break;
				}
			}
		} else {
			for (i = 0; i < sizeof(optable) / sizeof(optable[0]); i++) {
				int len = declp->p - declp->b - 3;
				if ((int)strlen(optable[i].in) == len && memcmp(optable[i].in, declp->b + 3, len) == 0) {
					string_clear(declp);
					string_append(declp, "operator");
					string_append(declp, optable[i].out);
					break;
				}
			}
		}
	} else if (declp->p - declp->b >= 5 && memcmp(declp->b, "type", 4) == 0 && strchr(cplus_markers, declp->b[4]) != NULL) {
		/* type conversion operator */
		tem = declp->b + 5;
		if (do_type(work, &tem, &type)) {
			string_clear(declp);
			string_append(declp, "operator ");
			string_appends(declp, &type);
			string_delete(&type);
		}
	} else if (declp->b[0] == '_' && declp->b[1] == '_' && declp->b[2] == 'o' && declp->b[3] == 'p') {
		/* ANSI.  */
		/* type conversion operator.  */
		tem = declp->b + 4;
		if (do_type(work, &tem, &type)) {
			string_clear(declp);
			string_append(declp, "operator ");
			string_appends(declp, &type);
			string_delete(&type);
		}
	} else if (declp->b[0] == '_' && declp->b[1] == '_' && declp->b[2] >= 'a' && declp->b[2] <= 'z' && declp->b[3] >= 'a' && declp->b[3] <= 'z') {
		if (declp->b[4] == '\0') {
			/* Operator.  */
			for (i = 0; i < sizeof(optable) / sizeof(optable[0]); i++) {
				if (strlen(optable[i].in) == 2 && memcmp(optable[i].in, declp->b + 2, 2) == 0) {
					string_clear(declp);
					string_append(declp, "operator");
					string_append(declp, optable[i].out);
					break;
				}
			}
		} else {
			if (declp->b[2] == 'a' && declp->b[5] == '\0') {
				/* Assignment.  */
				for (i = 0; i < sizeof(optable) / sizeof(optable[0]); i++) {
					if (strlen(optable[i].in) == 3 && memcmp(optable[i].in, declp->b + 2, 3) == 0) {
						string_clear(declp);
						string_append(declp, "operator");
						string_append(declp, optable[i].out);
						break;
					}
				}
			}
		}
	}
}

/* a mini string-handling package */

static void
	string_need(string *s, int n)
{
	int tem;

	if (s->b == NULL) {
		if (n < 32) {
			n = 32;
		}
		s->p = s->b = malloc(n);
		s->e = s->b + n;
	} else if (s->e - s->p < n) {
		tem = s->p - s->b;
		n += tem;
		n *= 2;
		s->b = realloc(s->b, n);
		s->p = s->b + tem;
		s->e = s->b + n;
	}
}

static void
	string_delete(string *s)
{
	if (s->b != NULL) {
		free(s->b);
		s->b = s->e = s->p = NULL;
	}
}

static void
	string_init(string *s)
{
	s->b = s->p = s->e = NULL;
}

static void
	string_clear(string *s)
{
	s->p = s->b;
}

#if 0

static int
string_empty (string *s)
{
  return (s->b == s->p);
}

#endif

static void
	string_append(string *p, const char *s)
{
	int n;
	if (s == NULL || *s == '\0')
		return;
	n = strlen(s);
	string_need(p, n);
	memcpy(p->p, s, n);
	p->p += n;
}

static void
	string_appends(string *p, string *s)
{
	int n;

	if (s->b != s->p) {
		n = s->p - s->b;
		string_need(p, n);
		memcpy(p->p, s->b, n);
		p->p += n;
	}
}

static void
	string_appendn(string *p, const char *s, int n)
{
	if (n != 0) {
		string_need(p, n);
		memcpy(p->p, s, n);
		p->p += n;
	}
}

static void
	string_prepend(string *p, const char *s)
{
	if (s != NULL && *s != '\0') {
		string_prependn(p, s, strlen(s));
	}
}

static void
	string_prepends(string *p, string *s)
{
	if (s->b != s->p) {
		string_prependn(p, s->b, s->p - s->b);
	}
}

static void
	string_prependn(string *p, const char *s, int n)
{
	char *q;

	if (n != 0) {
		string_need(p, n);
		for (q = p->p - 1; q >= p->b; q--) {
			q[n] = q[0];
		}
		memcpy(p->b, s, n);
		p->p += n;
	}
}
