#ifndef _RPNLOGIC_H_
#define _RPNLOGIC_H_

struct stack {
	double *v; /* element array */
	int n; /* used elements */
	int s; /* allocated elements */
};

struct rpn {
	struct rpn *next;
	int (*run)(struct stack *st, struct rpn *me);
	void *dat;
	char *topic;
	char *options;
	double value;
};

/* functions */
struct rpn *rpn_parse(const char *cstr, void *dat);

void rpn_stack_reset(struct stack *st);
int rpn_run(struct stack *st, struct rpn *rpn);

void rpn_free_chain(struct rpn *rpn);
void rpn_rebase(struct rpn *first, struct rpn **newptr);

/* imported function */
extern double rpn_lookup_env(const char *str, struct rpn *);
extern void rpn_run_again(void *dat);

#endif
