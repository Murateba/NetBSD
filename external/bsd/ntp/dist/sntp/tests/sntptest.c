/*	$NetBSD: sntptest.c,v 1.3 2024/08/18 20:47:26 christos Exp $	*/


#include "config.h"
#include "sntptest.h"

void
sntptest(void) {
	optionSaveState(&sntpOptions);
}


void
sntptest_destroy(void) {
	optionRestore(&sntpOptions);
}


void
ActivateOption(const char* option, const char* argument) {

	const int ARGV_SIZE = 4;

	char* opts[ARGV_SIZE];
	
	opts[0] = estrdup("sntpopts");
	opts[1] = estrdup(option);
	opts[2] = estrdup(argument);
	opts[3] = estrdup("127.0.0.1");

	optionProcess(&sntpOptions, COUNTOF(opts), opts);
}

