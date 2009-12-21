/*
 * Copyright (c) 2009 Ken McDonell.  All Rights Reserved.
 * 
 * This library is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published
 * by the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 * 
 * This library is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public
 * License for more details.
 * 
 * You should have received a copy of the GNU Lesser General Public License
 * along with this library; if not, write to the Free Software Foundation,
 * Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307 USA.
 *
 * Debug Flags
 *	DERIVE - high-level diagnostics
 *	DERIVE & APPL0 - configuration and static syntax analysis
 *	DERIVE & APPL1 - expression binding, semantic analysis, PMNS ops
 *	DERIVE & APPL2 - fetch handling
 *
 * Caveats
 * 1.	No derived metrics for pmFetchArchive() as this routine does
 *	not use a target pmidlist[]
 * 2.	Derived metrics will not work with pmRequestTraversePMNS() and
 *	pmReceiveTraversePMNS() because the by the time the list of
 *	names is received, the original name at the root of the search
 *	is no longer available.
 */

/*
 * TODO
 *
 *	if pmRegister called _after_ context is open, then iterate
 *	over all contexts and expand the ctl_t struct and call
 *	bind_expr and check_expr to add new dm for each context
 *
 *	Need to handle
 *	  pmRegister before any context is open
 *	  new contexts after pmRegister is called (the initial implementation)
 *	  intermixed pmRegister and newContext calls
 *
 *	pmUnregister - ?
 *
 * 	delta() support --  any other functions?
 *		 - map ctr to instant
 *
 *	skip blank lines in lexer/parser
 *
 */

#include "derive.h"

static int	need_init = 1;
static ctl_t	registered;

/* parser and lexer variables */
static char	*tokbuf = NULL;
static int	tokbuflen;
static char	*this;		/* start of current lexicon */
static int	lexpeek = 0;
static char	*string;
static char	*errmsg;

static char *type_dbg[] = { "ERROR", "EOF", "UNDEF", "NUMBER", "NAME", "EQUALS", "PLUS", "MINUS", "STAR", "SLASH", "LPAREN", "RPAREN", "DELTA" };
static char type_c[] = { '\0', '\0', '\0', '\0', '\0', '=', '+', '-', '*', '/', '(', ')', '\0' };

/* function table for lexer */
static struct {
    int		f_type;
    char	*f_name;
} func[] = {
    { L_DELTA,	"delta" },
    { L_UNDEF,	NULL }
};

/* parser states */
#define P_INIT		0
#define P_LEAF		1
#define P_LEAF_PAREN	2
#define P_BINOP		3
#define P_FUNC_OP	4
#define P_FUNC_END	5
#define P_END		99

static char *state_dbg[] = { "INIT", "LEAF", "LEAF_PAREN", "BINOP", "FUNC_OP", "FUNC_END" };

static void
init(void)
{
    char	*configpath;

    if ((configpath = getenv("PCP_DERIVED_CONFIG")) != NULL) {
	int	sts;
#ifdef PCP_DEBUG
	if (pmDebug & DBG_TRACE_DERIVE) {
	    fprintf(stderr, "Derived metric initialization from $PCP_DERIVED_CONFIG\n");
	}
#endif
	sts = pmLoadDerivedConfig(configpath);
#ifdef PCP_DEBUG
	if (sts < 0 && (pmDebug & DBG_TRACE_DERIVE)) {
	    fprintf(stderr, "pmLoadDerivedConfig -> %s\n", pmErrStr(sts));
	}
#endif
    }

    need_init = 0;
}

static void
unget(int c)
{
    lexpeek = c;
}

static int
get()
{
    static int	eof = 0;
    int		c;
    if (lexpeek != 0) {
	c = lexpeek;
	lexpeek = 0;
	return c;
    }
    if (eof) return L_EOF;
    c = *string;
    if (c == '\0') {
	return L_EOF;
	eof = 1;
    }
    string++;
    return c;
}

static int
lex(void)
{
    int		c;
    char	*p = tokbuf;
    int		ltype = L_UNDEF;
    int		i;
    int		firstch = 1;

    for ( ; ; ) {
	c = get();
	if (firstch) {
	    if (isspace(c)) continue;
	    this = &string[-1];
	    firstch = 0;
	}
	if (c == L_EOF) {
	    if (ltype != L_UNDEF) {
		/* force end of last token */
		c = 0;
	    }
	    else {
		/* really the end of the input */
		return L_EOF;
	    }
	}
	if (p == NULL) {
	    tokbuflen = 128;
	    if ((p = tokbuf = (char *)malloc(tokbuflen)) == NULL) {
		__pmNoMem("pmRegisterDerived: alloc tokbuf", tokbuflen, PM_FATAL_ERR);
		/*NOTREACHED*/
	    }
	}
	else if (p >= &tokbuf[tokbuflen]) {
	    int		x = p - tokbuf;
	    tokbuflen *= 2;
	    if ((tokbuf = (char *)realloc(tokbuf, tokbuflen)) == NULL) {
		__pmNoMem("pmRegisterDerived: realloc tokbuf", tokbuflen, PM_FATAL_ERR);
		/*NOTREACHED*/
	    }
	    p = &tokbuf[x];
	}

	*p++ = (char)c;

	if (ltype == L_UNDEF) {
	    if (isdigit(c))
		ltype = L_NUMBER;
	    else if (isalpha(c))
		ltype = L_NAME;
	    else {
		switch (c) {
		    case '=':
			*p = '\0';
			return L_EQUALS;
			break;

		    case '+':
			*p = '\0';
			return L_PLUS;
			break;

		    case '-':
			*p = '\0';
			return L_MINUS;
			break;

		    case '*':
			*p = '\0';
			return L_STAR;
			break;

		    case '/':
			*p = '\0';
			return L_SLASH;
			break;

		    case '(':
			*p = '\0';
			return L_LPAREN;
			break;

		    case ')':
			*p = '\0';
			return L_RPAREN;
			break;

		    default:
			return L_ERROR;
			break;
		}
	    }
	}
	else {
	    if (ltype == L_NUMBER) {
		if (!isdigit(c)) {
		    unget(c);
		    p[-1] = '\0';
		    return L_NUMBER;
		}
	    }
	    else if (ltype == L_NAME) {
		if (isalpha(c) || isdigit(c) || c == '_' || c == '.')
		    continue;
		if (c == '(') {
		    /* check for functions ... */
		    int		namelen = p - tokbuf - 1;
		    for (i = 0; func[i].f_name != NULL; i++) {
			if (namelen == strlen(func[i].f_name) &&
			    strncmp(tokbuf, func[i].f_name, namelen) == 0) {
			    *p = '\0';
			    return func[i].f_type;
			}
		    }
		}
		/* current character is end of name */
		unget(c);
		p[-1] = '\0';
		return L_NAME;
	    }
	}

    }
}

static node_t *
newnode(int type)
{
    node_t	*np;
    np = (node_t *)malloc(sizeof(node_t));
    if (np == NULL) {
	__pmNoMem("pmRegisterDerived: newnode", sizeof(node_t), PM_FATAL_ERR);
	/*NOTREACHED*/
    }
    np->type = type;
    np->save_last = 0;
    np->left = NULL;
    np->right = NULL;
    np->value = NULL;
    np->info = NULL;
    return np;
}

static void
free_expr(node_t *np)
{
    if (np == NULL) return;
    if (np->left != NULL) free_expr(np->left);
    if (np->right != NULL) free_expr(np->right);
    /* value is only allocated once for the static nodes */
    if (np->info == NULL && np->value != NULL) free(np->value);
    if (np->info != NULL) free(np->info);
    free(np);
}

/*
 * copy a static expression tree to make the dynamic per context
 * expression tree and initialize the info block
 */
static node_t *
bind_expr(int n, node_t *np)
{
    node_t	*new;

    assert(np != NULL);
    new = newnode(np->type);
    if (np->left != NULL) {
	if ((new->left = bind_expr(n, np->left)) == NULL) {
	    /* error, reported deeper in the recursion, clean up */
	    free(new);
	    return(NULL);
	}
    }
    if (np->right != NULL) {
	if ((new->right = bind_expr(n, np->right)) == NULL) {
	    /* error, reported deeper in the recursion, clean up */
	    free(new);
	    return(NULL);
	}
    }
    if ((new->info = (info_t *)malloc(sizeof(info_t))) == NULL) {
	__pmNoMem("bind_expr: info block", sizeof(info_t), PM_FATAL_ERR);
	/*NOTREACHED*/
    }
    new->info->pmid = PM_ID_NULL;
    new->info->numval = 0;
    new->info->ivlist = NULL;
    new->info->last_numval = 0;
    new->info->last_ivlist = NULL;

    /* need info to be non-null to protect copy of value in free_expr */
    new->value = np->value;

    new->save_last = np->save_last;

    if (new->type == L_NAME) {
	int	sts;

	sts = pmLookupName(1, &new->value, &new->info->pmid);
	if (sts < 0) {
	    pmprintf("Error: derived metric %s: operand: %s: %s\n", registered.mlist[n].name, new->value, pmErrStr(sts));
	    pmflush();
	    free(new->info);
	    free(new);
	    return NULL;
	}
	sts = pmLookupDesc(new->info->pmid, &new->desc);
	if (sts < 0) {
	    pmprintf("Error: derived metric %s: operand (%s [%s]): %s\n", registered.mlist[n].name, new->value, pmIDStr(new->info->pmid), pmErrStr(sts));
	    pmflush();
	    free(new->info);
	    free(new);
	    return NULL;
	}
    }
    else if (new->type == L_NUMBER) {
	new->desc = np->desc;
    }

    return new;
}

/* type promotion */
static int promote[6][6] = {
    { PM_TYPE_32, PM_TYPE_U32, PM_TYPE_64, PM_TYPE_U64, PM_TYPE_FLOAT, PM_TYPE_DOUBLE },
    { PM_TYPE_U32, PM_TYPE_U32, PM_TYPE_64, PM_TYPE_U64, PM_TYPE_FLOAT, PM_TYPE_DOUBLE },
    { PM_TYPE_64, PM_TYPE_64, PM_TYPE_64, PM_TYPE_U64, PM_TYPE_FLOAT, PM_TYPE_DOUBLE },
    { PM_TYPE_U64, PM_TYPE_U64, PM_TYPE_U64, PM_TYPE_U64, PM_TYPE_FLOAT, PM_TYPE_DOUBLE },
    { PM_TYPE_FLOAT, PM_TYPE_FLOAT, PM_TYPE_FLOAT, PM_TYPE_FLOAT, PM_TYPE_FLOAT, PM_TYPE_DOUBLE },
    { PM_TYPE_DOUBLE, PM_TYPE_DOUBLE, PM_TYPE_DOUBLE, PM_TYPE_DOUBLE, PM_TYPE_DOUBLE, PM_TYPE_DOUBLE }
};

static int
map_desc(int n, node_t *np)
{
    /*
     * pmDesc mapping for binary operators ...
     *
     * semantics		acceptable operators
     * counter, counter		+ -
     * non-counter, non-counter	+ - * /
     * counter, non-counter	* /
     * non-counter, counter	*
     *
     * type promotion (similar to ANSI C)
     * PM_TYPE_STRING, PM_TYPE_AGGREGATE and PM_TYPE_AGGREGATE_STATIC are
     * illegal operands except for renaming (no operator involved)
     * for the integer type operands, division => PM_TYPE_FLOAT
     * else PM_TYPE_DOUBLE & any type => PM_TYPE_DOUBLE
     * else PM_TYPE_FLOAT & any type => PM_TYPE_FLOAT
     * else PM_TYPE_U64 & any type => PM_TYPE_U64
     * else PM_TYPE_64 & any type => PM_TYPE_64
     * else PM_TYPE_U32 & any type => PM_TYPE_U32
     * else PM_TYPE_32 & any type => PM_TYPE_32
     *
     * units mapping
     * operator			checks
     * +, -			identical
     *				(TODO relax to union compatible and scale)
     * *, /			if one is counter, non-counter must
     *				have pmUnits of "none"
     */
    pmDesc	*right = &np->right->desc;
    pmDesc	*left = &np->left->desc;
    char	*errmsg;

    if (left->sem == PM_SEM_COUNTER) {
	if (right->sem == PM_SEM_COUNTER) {
	    if (np->type != L_PLUS && np->type != L_MINUS) {
		errmsg = "Illegal operator for counters";
		goto bad;
	    }
	}
	else {
	    if (np->type != L_STAR && np->type != L_SLASH) {
		errmsg = "Illegal operator for counter and non-counter";
		goto bad;
	    }
	}
    }
    else {
	if (right->sem == PM_SEM_COUNTER) {
	    if (np->type != L_STAR) {
		errmsg = "Illegal operator for non-counter and counter";
		goto bad;
	    }
	}
	else {
	    if (np->type != L_PLUS && np->type != L_MINUS &&
		np->type != L_STAR && np->type != L_SLASH) {
		/*
		 * this is not possible at the present since only
		 * arithmetic operators are supported and all are
		 * acceptable here ... check added for completeness
		 */
		errmsg = "Illegal operator for non-counters";
		goto bad;
	    }
	}
    }

    /*
     * Choose candidate descriptor ... prefer metric or expression
     * over constant
     */
    if (np->left->type != L_NUMBER)
	np->desc = *left;	/* struct copy */
    else
	np->desc = *right;	/* struct copy */

    /*
     * type checking and promotion
     */
    switch (left->type) {
	case PM_TYPE_32:
	case PM_TYPE_U32:
	case PM_TYPE_64:
	case PM_TYPE_U64:
	case PM_TYPE_FLOAT:
	case PM_TYPE_DOUBLE:
	    break;
	default:
	    errmsg = "Non-arithmetic type for left operand";
	    goto bad;
    }
    switch (right->type) {
	case PM_TYPE_32:
	case PM_TYPE_U32:
	case PM_TYPE_64:
	case PM_TYPE_U64:
	case PM_TYPE_FLOAT:
	case PM_TYPE_DOUBLE:
	    break;
	default:
	    errmsg = "Non-arithmetic type for right operand";
	    goto bad;
    }
    np->desc.type = promote[left->type][right->type];
    if (np->type == L_SLASH && np->desc.type != PM_TYPE_FLOAT && np->desc.type != PM_TYPE_DOUBLE) {
	/* for division result is real number */
	np->desc.type = PM_TYPE_FLOAT;
    }

    /* TODO finish off ... sem checking */
    return 0;

bad:
    pmprintf("Semantic error: derived metric %s: ", registered.mlist[n].name);
    if (np->left->type == L_NUMBER || np->left->type == L_NAME)
	pmprintf("%s ", np->left->value);
    else
	pmprintf("<expr> ");
    pmprintf("%c ", type_c[np->type+2]);
    if (np->right->type == L_NUMBER || np->right->type == L_NAME)
	pmprintf("%s", np->right->value);
    else
	pmprintf("<expr>");
    pmprintf(": %s\n", errmsg);
    pmflush();
    return -1;
}

static int
check_expr(int n, node_t *np)
{
    int		sts;

    assert(np != NULL);
    if (np->type == L_NUMBER || np->type == L_NAME)
	return 0;
    if (np->left != NULL)
	if ((sts = check_expr(n, np->left)) < 0)
	    return sts;
    if (np->right != NULL)
	if ((sts = check_expr(n, np->right)) < 0)
	    return sts;
    if (np->left == NULL) {
	np->desc = np->right->desc;	/* struct copy */
    }
    else if (np->right == NULL) {
	np->desc = np->left->desc;	/* struct copy */
	/*
	 * special cases for functions ...
	 * delta	expect counter operand, result is instantaneous
	 */
	if (np->type == L_DELTA) {
	    /* TODO check counter and check type is numeric */
	    np->desc.sem = PM_SEM_INSTANT;
	}
    }
    else {
	/* build pmDesc from pmDesc of both operands */
	if ((sts = map_desc(n, np)) < 0) {
	    return sts;
	}
    }
    return 0;
}

static void
dump_value(int type, pmAtomValue *avp)
{
    switch (type) {
	case PM_TYPE_32:
	    fprintf(stderr, "%i", avp->l);
	    break;

	case PM_TYPE_U32:
	    fprintf(stderr, "%u", avp->ul);
	    break;

	case PM_TYPE_64:
	    fprintf(stderr, "%lli", (long long)avp->ll);
	    break;

	case PM_TYPE_U64:
	    fprintf(stderr, "%llu", (unsigned long long)avp->ull);
	    break;

	case PM_TYPE_FLOAT:
	    fprintf(stderr, "%g", (double)avp->f);
	    break;

	case PM_TYPE_DOUBLE:
	    fprintf(stderr, "%g", avp->d);
	    break;

	case PM_TYPE_STRING:
	    fprintf(stderr, "%s", avp->cp);
	    break;

	case PM_TYPE_AGGREGATE:
	case PM_TYPE_UNKNOWN:
	    fprintf(stderr, "[blob]");
	    break;

	case PM_TYPE_NOSUPPORT:
	    fprintf(stderr, "dump_value: bogus value, metric Not Supported\n");
	    break;

	default:
	    fprintf(stderr, "dump_value: unknown value type=%d\n", type);
    }
}

void
__dmdumpexpr(node_t *np, int level)
{
    if (level == 0) fprintf(stderr, "Derived metric expr dump from %p...\n", np);
    if (np == NULL) return;
    fprintf(stderr, "expr node %p type=%s left=%p right=%p save_last=%d", np, type_dbg[np->type+2], np->left, np->right, np->save_last);
    if (np->type == L_NAME || np->type == L_NUMBER)
	fprintf(stderr, " [%s] master=%d", np->value, np->info == NULL ? 1 : 0);
    fputc('\n', stderr);
    if (np->info) {
	fprintf(stderr, "    PMID: %s (%s from pmDesc) numval: %d [%s]\n", pmIDStr(np->info->pmid), pmIDStr(np->desc.pmid), np->info->numval, np->info->iv_alloc ? "allocated" : "copied");
	__pmPrintDesc(stderr, &np->desc);
	if (np->info->ivlist) {
	    int		j;
	    int		max;

	    max = np->info->numval > np->info->last_numval ? np->info->numval : np->info->last_numval;

	    for (j = 0; j < max; j++) {
		fprintf(stderr, "[%d]", j);
		if (j < np->info->numval) {
		    fprintf(stderr, " inst=%d, val=", np->info->ivlist[j].inst);
		    dump_value(np->desc.type, &np->info->ivlist[j].value);
		}
		if (j < np->info->last_numval) {
		    fprintf(stderr, " (last inst=%d, val=", np->info->last_ivlist[j].inst);
		    dump_value(np->desc.type, &np->info->last_ivlist[j].value);
		    fputc(')', stderr);
		}
		fputc('\n', stderr);
	    }
	}
    }
    if (np->left != NULL) __dmdumpexpr(np->left, level+1);
    if (np->right != NULL) __dmdumpexpr(np->right, level+1);
}

/*
 * Parser FSA
 * state	lex		new state
 * P_INIT	L_NAME or	P_LEAF
 * 		L_NUMBER
 * P_INIT	L_<func>	P_FUNC_OP
 * P_INIT	L_LPAREN	if parse() != NULL then P_LEAF
 * P_LEAF	L_PLUS or	P_BINOP
 * 		L_MINUS or
 * 		L_STAR or
 * 		L_SLASH
 * P_BINOP	L_NAME or	P_LEAF
 * 		L_NUMBER
 * P_BINOP	L_LPAREN	if parse() != NULL then P_LEAF
 * P_LEAF_PAREN	same as P_LEAF, but no precedence rules at next operator
 * P_FUNC_OP	L_NAME		P_FUNC_END
 * P_FUNC_END	L_RPAREN	P_LEAF
 */
static node_t *
parse(int level)
{
    int		state = P_INIT;
    int		type;
    node_t	*expr = NULL;
    node_t	*curr = NULL;
    node_t	*np;

    for ( ; ; ) {
	type = lex();
#ifdef PCP_DEBUG
	if ((pmDebug & DBG_TRACE_DERIVE) && (pmDebug & DBG_TRACE_APPL0)) {
	    fprintf(stderr, "parse(%d) state=P_%s type=L_%s \"%s\"\n", level, state_dbg[state], type_dbg[type+2], type == L_EOF ? "" : tokbuf);
	}
#endif
	/* handle lexicons that terminate the parsing */
	switch (type) {
	    case L_ERROR:
		errmsg = "Illegal character";
		free_expr(expr);
		return NULL;
		break;
	    case L_EOF:
		if (state == P_LEAF || state == P_LEAF_PAREN)
		    return expr;
		errmsg = "End of input";
		free_expr(expr);
		return NULL;
		break;
	    case L_RPAREN:
		if (state == P_LEAF || state == P_LEAF_PAREN || state == P_FUNC_END)
		    return expr;
		errmsg = "Unexpected ')'";
		free_expr(expr);
		return NULL;
		break;
	}

	switch (state) {
	    case P_INIT:
		if (type == L_NAME || type == L_NUMBER) {
		    expr = curr = newnode(type);
		    if ((curr->value = strdup(tokbuf)) == NULL) {
			__pmNoMem("pmRegisterDerived: leaf node", strlen(tokbuf)+1, PM_FATAL_ERR);
			/*NOTREACHED*/
		    }
		    if (type == L_NUMBER) {
			/* TODO check value is small enough to fit in a 32-bit int */
			curr->desc.pmid = PM_ID_NULL;
			curr->desc.type = PM_TYPE_U32;
			curr->desc.indom = PM_INDOM_NULL;
			curr->desc.sem = PM_SEM_DISCRETE;
			memset(&curr->desc.units, 0, sizeof(pmUnits));
		    }
		    state = P_LEAF;
		}
		else if (type == L_LPAREN) {
		    expr = curr = parse(level+1);
		    if (expr == NULL)
			return NULL;
		    state = P_LEAF_PAREN;
		}
		else if (type == L_DELTA) {
		    expr = curr = newnode(type);
		    state = P_FUNC_OP;
		}
		else
		    return NULL;
		break;

	    case P_LEAF_PAREN:	/* fall through */
	    case P_LEAF:
		if (type == L_PLUS || type == L_MINUS || type == L_STAR || type == L_SLASH) {
		    np = newnode(type);
		    if (state == P_LEAF_PAREN ||
		        (curr->type == L_NAME || curr->type == L_NUMBER) ||
		        (type == L_PLUS || type == L_MINUS)) {
			/*
			 * first operator or equal or lower precedence
			 * make new root of tree and push previous
			 * expr down left descendent branch
			 */
			np->left = curr;
			expr = curr = np;
		    }
		    else {
			/*
			 * push previous right branch down one level
			 */
			np->left = curr->right;
			curr->right = np;
			curr = np;
		    }
		    state = P_BINOP;
		}
		else {
		    free_expr(expr);
		    return NULL;
		}
		break;

	    case P_BINOP:
		if (type == L_NAME || type == L_NUMBER) {
		    np = newnode(type);
		    if ((np->value = strdup(tokbuf)) == NULL) {
			__pmNoMem("pmRegisterDerived: leaf node", strlen(tokbuf)+1, PM_FATAL_ERR);
			/*NOTREACHED*/
		    }
		    if (type == L_NUMBER) {
			np->desc.pmid = PM_ID_NULL;
			np->desc.type = PM_TYPE_U32;
			np->desc.indom = PM_INDOM_NULL;
			np->desc.sem = PM_SEM_DISCRETE;
			memset(&np->desc.units, 0, sizeof(pmUnits));
		    }
		    curr->right = np;
		    curr = expr;
		    state = P_LEAF;
		}
		else if (type == L_LPAREN) {
		    np = parse(level+1);
		    if (np == NULL)
			return NULL;
		    curr->right = np;
		    state = P_LEAF_PAREN;
		}
		else {
		    free_expr(expr);
		    return NULL;
		}
		break;

	    case P_FUNC_OP:
		if (type == L_NAME) {
		    np = newnode(type);
		    if ((np->value = strdup(tokbuf)) == NULL) {
			__pmNoMem("pmRegisterDerived: func op node", strlen(tokbuf)+1, PM_FATAL_ERR);
			/*NOTREACHED*/
		    }
		    np->save_last = 1;
		    curr->left = np;
		    state = P_FUNC_END;
		}
		else {
		    free_expr(expr);
		    return NULL;
		}
		break;

	    default:
		free_expr(expr);
		return NULL;
	}
    }
}

static int
checkname(char *p)
{
    int	firstch = 1;

    for ( ; *p; p++) {
	if (firstch) {
	    firstch = 0;
	    if (isalpha(*p)) continue;
	    return -1;
	}
	else {
	    if (isalpha(*p) || isdigit(*p) || *p == '_') continue;
	    if (*p == '.') {
		firstch = 1;
		continue;
	    }
	    return -1;
	}
    }
    return 0;
}

char *
pmRegisterDerived(char *name, char *expr)
{
    node_t		*np;
    static __pmID_int	pmid;

#ifdef PCP_DEBUG
    if ((pmDebug & DBG_TRACE_DERIVE) && (pmDebug & DBG_TRACE_APPL0)) {
	fprintf(stderr, "pmRegisterDerived: name=\"%s\" expr=\"%s\"\n", name, expr);
    }
#endif

    /* TODO check for and reject duplicate names */

    errmsg = NULL;
    string = expr;
    np = parse(1);
    if (np == NULL) {
	/* parser error */
	return this;
    }

    registered.nmetric++;
    registered.mlist = (dm_t *)realloc(registered.mlist, registered.nmetric*sizeof(dm_t));
    if (registered.mlist == NULL) {
	__pmNoMem("pmRegisterDerived: registered mlist", registered.nmetric*sizeof(dm_t), PM_FATAL_ERR);
	/*NOTREACHED*/
    }
    if (registered.nmetric == 1) {
	pmid.flag = 0;
	pmid.domain = DYNAMIC_PMID;
	pmid.cluster = 0;
    }
    registered.mlist[registered.nmetric-1].name = strdup(name);
    pmid.item = registered.nmetric;
    registered.mlist[registered.nmetric-1].pmid = *((pmID *)&pmid);
    registered.mlist[registered.nmetric-1].expr = np;

#ifdef PCP_DEBUG
    if (pmDebug & DBG_TRACE_DERIVE) {
	fprintf(stderr, "pmRegisterDerived: register metric[%d] %s = %s\n", registered.nmetric-1, name, expr);
	if (pmDebug & DBG_TRACE_APPL0)
	    __dmdumpexpr(np, 0);
    }
#endif

    return NULL;
}

int
pmLoadDerivedConfig(char *fname)
{
    FILE	*fp;
    int		buflen;
    char	*buf;
    char	*p;
    int		c;
    int		sts = 0;
    int		eq = -1;
    int		lineno = 1;

#ifdef PCP_DEBUG
    if (pmDebug & DBG_TRACE_DERIVE) {
	fprintf(stderr, "pmLoadDerivedConfig(\"%s\")\n", fname);
    }
#endif

    if ((fp = fopen(fname, "r")) == NULL) {
	return -errno;
    }
    buflen = 128;
    if ((buf = (char *)malloc(buflen)) == NULL) {
	__pmNoMem("pmLoadDerivedConfig: alloc buf", buflen, PM_FATAL_ERR);
	/*NOTREACHED*/
    }
    p = buf;
    while ((c = fgetc(fp)) != EOF) {
	if (p == &buf[buflen]) {
	    if ((buf = (char *)realloc(buf, 2*buflen)) == NULL) {
		__pmNoMem("pmLoadDerivedConfig: expand buf", 2*buflen, PM_FATAL_ERR);
		/*NOTREACHED*/
	    }
	    p = &buf[buflen];
	    buflen *= 2;
	}
	if (c == '=' && eq == -1) {
	    /*
	     * mark first = in line ... metric name to the left and
	     * expression to the right
	     */
	    eq = p - buf;
	}
	if (c == '\n') {
	    if (buf[0] == '#') {
		/* comment line, skip it ... */
		goto next_line;
	    }
	    *p = '\0';
	    if (eq != -1) {
		char	*np;	/* copy of name */
		char	*ep;	/* start of expression */
		char	*q;
		char	*errp;
		buf[eq] = '\0';
		if ((np = strdup(buf)) == NULL) {
		    __pmNoMem("pmLoadDerivedConfig: dupname", strlen(buf), PM_FATAL_ERR);
		    /*NOTREACHED*/
		}
		/* trim white space from tail of metric name */
		q = &np[eq-1];
		while (q >= np && isspace(*q))
		    *q-- = '\0';
		/* trim white space from head of metric name */
		q = np;
		while (*q && isspace(*q))
		    q++;
		if (*q == '\0') {
		    buf[eq] = '=';
		    pmprintf("[%s:%d] Error: pmLoadDerivedConfig: derived metric name missing\n%s\n", fname, lineno, buf);
		    pmflush();
		    free(np);
		    goto next_line;
		}
		if (checkname(q) < 0) {
		    pmprintf("[%s:%d] Error: pmLoadDerivedConfig: illegal derived metric name (%s)\n", fname, lineno, q);
		    pmflush();
		    free(np);
		    goto next_line;
		}
		ep = &buf[eq+1];
		while (*ep != '\0' && isspace(*ep))
		    ep++;
		if (*ep == '\0') {
		    buf[eq] = '=';
		    pmprintf("[%s:%d] Error: pmLoadDerivedConfig: expression missing\n%s\n", fname, lineno, buf);
		    pmflush();
		    free(np);
		    goto next_line;
		}
		errp = pmRegisterDerived(q, ep);
		if (errp != NULL) {
		    pmprintf("[%s:%d] Error: pmRegisterDerived(%s, ...) syntax error\n", fname, lineno, q);
		    pmprintf("%s\n", &buf[eq+1]);
		    for (q = &buf[eq+1]; *q; q++) {
			if (q == errp) *q = '^';
			else if (!isspace(*q)) *q = ' ';
		    }
		    pmprintf("%s\n", &buf[eq+1]);
		    q = pmDerivedErrStr();
		    if (q != NULL) pmprintf("%s\n", q);
		    pmflush();
		}
		else
		    sts++;
		free(np);
	    }
	    else {
		/*
		 * error ... no = in the line, so no derived metric name
		 */
		pmprintf("[%s:%d] Error: pmLoadDerivedConfig: missing ``='' after derived metric name\n%s\n", fname, lineno, buf);
		pmflush();
	    }
next_line:
	    lineno++;
	    p = buf;
	    eq = -1;
	}
	else
	    *p++ = c;
    }
    return sts;
}

char *
pmDerivedErrStr(void)
{
    return errmsg;
}

/*
 * callbacks
 */

int
__dmtraverse(const char *name, char ***namelist)
{
    int		sts = 0;
    int		i;
    char	**list = NULL;
    int		matchlen = strlen(name);

    if (need_init) init();

    for (i = 0; i < registered.nmetric; i++) {
	/*
	 * prefix match ... if name is "", then all names match
	 */
	if (matchlen == 0 ||
	    (strncmp(name, registered.mlist[i].name, matchlen) == 0 &&
	     (registered.mlist[i].name[matchlen] == '.' ||
	      registered.mlist[i].name[matchlen] == '\0'))) {
	    sts++;
	    if ((list = (char **)realloc(list, sts*sizeof(list[0]))) == NULL) {
		__pmNoMem("__dmtraverse: list", sts*sizeof(list[0]), PM_FATAL_ERR);
		/*NOTREACHED*/
	    }
	    list[sts-1] = registered.mlist[i].name;
	}
    }
    *namelist = list;

    return sts;
}

int
__dmchildren(const char *name, char ***offspring, int **statuslist)
{
    int		sts = 0;
    int		i;
    int		j;
    char	**children = NULL;
    int		*status = NULL;
    int		matchlen = strlen(name);
    int		start;
    int		len;

    if (need_init) init();

    for (i = 0; i < registered.nmetric; i++) {
	/*
	 * prefix match ... pick off the unique next level names on match
	 */
	if (name[0] == '\0' ||
	    (strncmp(name, registered.mlist[i].name, matchlen) == 0 &&
	     (registered.mlist[i].name[matchlen] == '.' ||
	      registered.mlist[i].name[matchlen] == '\0'))) {
	    if (registered.mlist[i].name[matchlen] == '\0') {
		/* leaf node */
		return 0;
	    }
	    start = matchlen > 0 ? matchlen + 1 : 0;
	    for (j = 0; j < sts; j++) {
		len = strlen(children[j]);
		if (strncmp(&registered.mlist[i].name[start], children[j], len) == 0 &&
		    registered.mlist[i].name[start+len] == '.')
		    break;
	    }
	    if (j == sts) {
		/* first time for this one */
		sts++;
		if ((children = (char **)realloc(children, sts*sizeof(children[0]))) == NULL) {
		    __pmNoMem("__dmchildren: children", sts*sizeof(children[0]), PM_FATAL_ERR);
		    /*NOTREACHED*/
		}
		for (len = 0; registered.mlist[i].name[start+len] != '\0' && registered.mlist[i].name[start+len] != '.'; len++)
		    ;
		if ((children[sts-1] = (char *)malloc(len+1)) == NULL) {
		    __pmNoMem("__dmchildren: name", len+1, PM_FATAL_ERR);
		    /*NOTREACHED*/
		}
		strncpy(children[sts-1], &registered.mlist[i].name[start], len);
		children[sts-1][len] = '\0';
#ifdef PCP_DEBUG
		if ((pmDebug & DBG_TRACE_DERIVE) && (pmDebug & DBG_TRACE_APPL1)) {
		    fprintf(stderr, "__dmchildren: offspring[%d] %s", sts-1, children[sts-1]);
		}
#endif

		if (statuslist != NULL) {
		    if ((status = (int *)realloc(status, sts*sizeof(status[0]))) == NULL) {
			__pmNoMem("__dmchildren: statrus", sts*sizeof(status[0]), PM_FATAL_ERR);
			/*NOTREACHED*/
		    }
		    status[sts-1] = registered.mlist[i].name[start+len] == '\0' ? PMNS_LEAF_STATUS : PMNS_NONLEAF_STATUS;
#ifdef PCP_DEBUG
		    if ((pmDebug & DBG_TRACE_DERIVE) && (pmDebug & DBG_TRACE_APPL1)) {
			fprintf(stderr, " (status=%d)", status[sts-1]);
		}
#endif
		}
#ifdef PCP_DEBUG
		if ((pmDebug & DBG_TRACE_DERIVE) && (pmDebug & DBG_TRACE_APPL1)) {
		    fputc('\n', stderr);
		}
#endif
	    }
	}
    }

    if (sts == 0)
	return PM_ERR_NAME;

    *offspring = children;
    if (statuslist != NULL)
	*statuslist = status;

    return sts;
}

int
__dmgetpmid(const char *name, pmID *dp)
{
    int		i;

    if (need_init) init();

    for (i = 0; i < registered.nmetric; i++) {
	if (strcmp(name, registered.mlist[i].name) == 0) {
	    *dp = registered.mlist[i].pmid;
	    return 0;
	}
    }
    return PM_ERR_NAME;
}

int
__dmgetname(pmID pmid, char ** name)
{
    int		i;

    if (need_init) init();

    for (i = 0; i < registered.nmetric; i++) {
	if (pmid == registered.mlist[i].pmid) {
	    *name = strdup(registered.mlist[i].name);
	    if (*name == NULL)
		return -errno;
	    else
		return 0;
	}
    }
    return PM_ERR_PMID;
}

void
__dmopencontext(__pmContext *ctxp)
{
    int		i;
    int		sts;
    ctl_t	*cp;

    if (need_init) init();

#ifdef PCP_DEBUG
    if ((pmDebug & DBG_TRACE_DERIVE) && (pmDebug & DBG_TRACE_APPL1)) {
	fprintf(stderr, "__dmopencontext() called\n");
    }
#endif
    if (registered.nmetric == 0) {
	ctxp->c_dm = NULL;
	return;
    }
    if ((cp = (void *)malloc(sizeof(ctl_t))) == NULL) {
	__pmNoMem("pmNewContext: derived metrics (ctl)", sizeof(ctl_t), PM_FATAL_ERR);
	/* NOTREACHED */
    }
    ctxp->c_dm = (void *)cp;
    cp->nmetric = registered.nmetric;
    if ((cp->mlist = (dm_t *)malloc(cp->nmetric*sizeof(dm_t))) == NULL) {
	__pmNoMem("pmNewContext: derived metrics (mlist)", cp->nmetric*sizeof(dm_t), PM_FATAL_ERR);
	/* NOTREACHED */
    }
    for (i = 0; i < cp->nmetric; i++) {
	cp->mlist[i].name = registered.mlist[i].name;
	cp->mlist[i].pmid = registered.mlist[i].pmid;
	if (registered.mlist[i].expr != NULL) {
	    /* failures must be reported in bind_expr() or below */
	    cp->mlist[i].expr = bind_expr(i, registered.mlist[i].expr);
	    if (cp->mlist[i].expr != NULL) {
		/* failures must be reported in check_expr() or below */
		sts = check_expr(i, cp->mlist[i].expr);
		if (sts < 0) {
		    free_expr(cp->mlist[i].expr);
		    cp->mlist[i].expr = NULL;
		}
		else {
		    /* set correct PMID in pmDesc at the top level */
		    cp->mlist[i].expr->desc.pmid = cp->mlist[i].pmid;
		}
	    }
	}
	else
	    cp->mlist[i].expr = NULL;
#ifdef PCP_DEBUG
	if ((pmDebug & DBG_TRACE_DERIVE) && cp->mlist[i].expr != NULL) {
	    fprintf(stderr, "__dmopencontext: bind metric[%d] %s\n", i, registered.mlist[i].name);
	    if (pmDebug & DBG_TRACE_APPL1)
		__dmdumpexpr(cp->mlist[i].expr, 0);
	}
#endif
    }
}

void
__dmclosecontext(__pmContext *ctxp)
{
    int		i;
    ctl_t	*cp = (ctl_t *)ctxp->c_dm;

    /* if needed, init() called in __dmopencontext beforehand */

#ifdef PCP_DEBUG
    if (pmDebug & DBG_TRACE_DERIVE) {
	fprintf(stderr, "__dmclosecontext() called dm->%p %d metrics\n", cp, cp == NULL ? -1 : cp->nmetric);
    }
#endif
    if (cp == NULL) return;
    for (i = 0; i < cp->nmetric; i++) {
	free_expr(cp->mlist[i].expr); 
    }
    free(cp->mlist);
    free(cp);
    ctxp->c_dm = NULL;
}

int
__dmdesc(__pmContext *ctxp, pmID pmid, pmDesc *desc)
{
    int		i;
    ctl_t	*cp = (ctl_t *)ctxp->c_dm;

    /* if needed, init() called in __dmopencontext beforehand */

    if (cp == NULL) return PM_ERR_PMID;

    for (i = 0; i < cp->nmetric; i++) {
	if (cp->mlist[i].pmid == pmid) {
	    if (cp->mlist[i].expr == NULL)
		/* bind failed for some reason, reported earlier */
		return PM_ERR_NAME;
	    *desc = cp->mlist[i].expr->desc;
	    return 0;
	}
    }
    return PM_ERR_PMID;
}
