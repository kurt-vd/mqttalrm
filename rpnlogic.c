#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <syslog.h>

#include "rpnlogic.h"

#define mylog(loglevel, fmt, ...) \
	({\
		syslog(loglevel, fmt, ##__VA_ARGS__); \
		if (loglevel <= LOG_ERR)\
			exit(1);\
	})
#define ESTR(num)	strerror(num)

/* manage */
static struct rpn *rpn_create(void)
{
	struct rpn *rpn;

	rpn = malloc(sizeof(*rpn));
	if (!rpn)
		mylog(LOG_ERR, "malloc failed?");
	memset(rpn, 0, sizeof(*rpn));
	return rpn;
}

static void rpn_free(struct rpn *rpn)
{
	if (rpn->topic)
		free(rpn->topic);
	free(rpn);
}

void rpn_free_chain(struct rpn *rpn)
{
	struct rpn *tmp;

	while (rpn) {
		tmp = rpn;
		rpn = rpn->next;
		rpn_free(tmp);
	}
}

/* algebra */
static int rpn_do_plus(struct stack *st, struct rpn *me)
{
	if (st->n < 2)
		/* stack underflow */
		return -1;
	st->v[st->n-2] = st->v[st->n-2] + st->v[st->n-1];
	st->n -= 1;
	return 0;
}
static int rpn_do_minus(struct stack *st, struct rpn *me)
{
	if (st->n < 2)
		/* stack underflow */
		return -1;
	st->v[st->n-2] = st->v[st->n-2] - st->v[st->n-1];
	st->n -= 1;
	return 0;
}
static int rpn_do_mul(struct stack *st, struct rpn *me)
{
	if (st->n < 2)
		/* stack underflow */
		return -1;
	st->v[st->n-2] = st->v[st->n-2] * st->v[st->n-1];
	st->n -= 1;
	return 0;
}
static int rpn_do_div(struct stack *st, struct rpn *me)
{
	if (st->n < 2)
		/* stack underflow */
		return -1;
	st->v[st->n-2] = st->v[st->n-2] / st->v[st->n-1];
	st->n -= 1;
	return 0;
}
static int rpn_do_pow(struct stack *st, struct rpn *me)
{
	if (st->n < 2)
		/* stack underflow */
		return -1;
	st->v[st->n-2] = pow(st->v[st->n-2], st->v[st->n-1]);
	st->n -= 1;
	return 0;
}

/* bitwise */
static int rpn_do_bitand(struct stack *st, struct rpn *me)
{
	if (st->n < 2)
		/* stack underflow */
		return -1;
	st->v[st->n-2] = (int)st->v[st->n-2] & (int)st->v[st->n-1];
	st->n -= 1;
	return 0;
}
static int rpn_do_bitor(struct stack *st, struct rpn *me)
{
	if (st->n < 2)
		/* stack underflow */
		return -1;
	st->v[st->n-2] = (int)st->v[st->n-2] | (int)st->v[st->n-1];
	st->n -= 1;
	return 0;
}
static int rpn_do_bitxor(struct stack *st, struct rpn *me)
{
	if (st->n < 2)
		/* stack underflow */
		return -1;
	st->v[st->n-2] = (int)st->v[st->n-2] ^ (int)st->v[st->n-1];
	st->n -= 1;
	return 0;
}
static int rpn_do_bitinv(struct stack *st, struct rpn *me)
{
	if (st->n < 1)
		/* stack underflow */
		return -1;
	st->v[st->n-1] = ~(int)st->v[st->n-1];
	return 0;
}

/* boolean */
static int rpn_do_booland(struct stack *st, struct rpn *me)
{
	if (st->n < 2)
		/* stack underflow */
		return -1;
	st->v[st->n-2] = (int)st->v[st->n-2] && (int)st->v[st->n-1];
	st->n -= 1;
	return 0;
}
static int rpn_do_boolor(struct stack *st, struct rpn *me)
{
	if (st->n < 2)
		/* stack underflow */
		return -1;
	st->v[st->n-2] = (int)st->v[st->n-2] || (int)st->v[st->n-1];
	st->n -= 1;
	return 0;
}
static int rpn_do_boolnot(struct stack *st, struct rpn *me)
{
	if (st->n < 1)
		/* stack underflow */
		return -1;
	st->v[st->n-1] = !(int)st->v[st->n-1];
	return 0;
}

/* compare */
static int rpn_do_lt(struct stack *st, struct rpn *me)
{
	if (st->n < 2)
		/* stack underflow */
		return -1;
	st->v[st->n-2] = st->v[st->n-2] < (int)st->v[st->n-1];
	st->n -= 1;
	return 0;
}
static int rpn_do_gt(struct stack *st, struct rpn *me)
{
	if (st->n < 2)
		/* stack underflow */
		return -1;
	st->v[st->n-2] = st->v[st->n-2] > (int)st->v[st->n-1];
	st->n -= 1;
	return 0;
}

/* generic */
static void rpn_push(struct stack *st, double value)
{
	if (st->n >= st->s) {
		st->s += 16;
		st->v = realloc(st->v, st->s * sizeof(st->v[0]));
		if (!st->v)
			mylog(LOG_ERR, "realloc stack %u failed", st->s);
	}
	st->v[st->n++] = value;
}

static int rpn_do_const(struct stack *st, struct rpn *me)
{
	rpn_push(st, me->value);
	return 0;
}

static int rpn_do_env(struct stack *st, struct rpn *me)
{
	rpn_push(st, rpn_lookup_env(me->topic));
	return 0;
}

static int rpn_do_dup(struct stack *st, struct rpn *me)
{
	if (st->n < 1)
		/* stack underflow */
		return -1;
	rpn_push(st, st->v[st->n-1]);
	return 0;
}
static int rpn_do_swap(struct stack *st, struct rpn *me)
{
	double tmp;

	if (st->n < 2)
		/* stack underflow */
		return -1;
	tmp = st->v[st->n-2];
	st->v[st->n-2] = st->v[st->n-1];
	st->v[st->n-1] = tmp;
	return 0;
}

/* run time functions */
void rpn_stack_reset(struct stack *st)
{
	st->n = 0;
}

int rpn_run(struct stack *st, struct rpn *rpn)
{
	int ret;

	for (; rpn; rpn = rpn->next) {
		ret = rpn->run(st, rpn);
		if (ret < 0)
			return ret;
	}
	return 0;
}

/* parser */
static struct lookup {
	char str[4];
	int (*run)(struct stack *, struct rpn *);
} const lookups[] = {
	{ "+", rpn_do_plus, },
	{ "-", rpn_do_minus, },
	{ "*", rpn_do_mul, },
	{ "/", rpn_do_div, },
	{ "**", rpn_do_pow, },

	{ "&", rpn_do_bitand, },
	{ "|", rpn_do_bitor, },
	{ "^", rpn_do_bitxor, },
	{ "~", rpn_do_bitinv, },

	{ "&&", rpn_do_booland, },
	{ "||", rpn_do_boolor, },
	{ "!", rpn_do_boolnot, },

	{ "<", rpn_do_lt, },
	{ ">", rpn_do_gt, },

	{ "dup", rpn_do_dup, },
	{ "swap", rpn_do_swap, },
	{},
};

static const struct lookup *do_lookup(const char *tok)
{
	const struct lookup *lookup;

	for (lookup = lookups; lookup->str[0]; ++lookup) {
		if (!strcmp(lookup->str, tok))
			return lookup;
	}
	return NULL;
}

static const char digits[] = "0123456789";
struct rpn *rpn_parse(const char *cstr)
{
	char *savedstr;
	char *tok;
	struct rpn *root = NULL, *last = NULL, *rpn;
	const struct lookup *lookup;

	savedstr = strdup(cstr);
	for (tok = strtok(savedstr, " \t"); tok; tok = strtok(NULL, " \t")) {
		if (strchr(digits, *tok) || (tok[1] && strchr("+-", *tok) && strchr(digits, tok[1]))) {
			rpn = rpn_create();
			rpn->run = rpn_do_const;
			rpn->value = strtod(tok, NULL);

		} else if (tok[0] == '$' && tok[1] == '{' && tok[strlen(tok)-1] == '}') {
			rpn = rpn_create();
			rpn->run = rpn_do_env;
			rpn->topic = strndup(tok+2, strlen(tok+2)-1);

		} else if ((lookup = do_lookup(tok)) != NULL) {
			rpn = rpn_create();
			rpn->run = lookup->run;

		} else {
			mylog(LOG_INFO, "unknown token '%s'", tok);
			if (root)
				rpn_free_chain(root);
			root = NULL;
			break;
		}
		if (last)
			last->next = rpn;
		if (!root)
			root = rpn;
		last = rpn;
	}
	free(savedstr);
	return root;
}
