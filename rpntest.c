#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <syslog.h>

#include "rpnlogic.h"

static struct stack rpnstack;

double rpn_lookup_env(const char *str, struct rpn *rpn)
{
	return strtod(getenv(str) ?: "0", NULL);
}
void rpn_run_again(void *dat)
{
}

int main(int argc, char *argv[])
{
	struct rpn *rpn;
	char *input, *fmt;

	openlog("rpntest", LOG_PERROR, LOG_LOCAL2);
	input = argv[1];
	fmt = strrchr(input, ' ');
	if (fmt && fmt[1] == '%')
		*fmt++ = 0;
	rpn = rpn_parse(input, NULL);
	if (!rpn)
		return 1;
	if (rpn_run(&rpnstack, rpn))
		return 1;
	if (rpnstack.n != 1)
		printf("rpn left %u items\n", rpnstack.n);
	printf(fmt ?: "%lf", rpnstack.v[rpnstack.n-1]);
	printf("\n");
	return 0;
}
