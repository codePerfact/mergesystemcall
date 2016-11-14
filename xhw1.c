#include <asm/unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <getopt.h>
#include "xmerge.h"

#ifndef __NR_xmergesort
#error xmergesort system call not defined
#endif

int main(int argc, char **argv)
{
	int err;
	int option;
	fileinput *input;
	input = malloc(sizeof(struct input));
	if (!input) {
		printf("[main] : MALLOC FAILED");
		err = -ENOMEM;
		goto out_ok;
	}

	while ((option = getopt(argc, argv, "uaitd")) != -1) {
		switch (option) {
		case 'u':
			input->flags = input->flags | 0x01;
			break;
		case 'a':
			input->flags = input->flags | 0x02;
			break;
		case 'i':
			input->flags = input->flags | 0x04;
			break;
		case 't':
			input->flags = input->flags | 0x10;
			break;
		case 'd':
			input->flags = input->flags | 0x20;
			break;
		default:
			err = -1;
			printf("[main] : Invalid option %c\n", option);
			goto out;
		}
	}

	if ((optind + 3) > argc) {
		printf("[main] : Inappropriate number of arguments\n");
		goto out;
	}
	input->outfile = argv[optind];
	input->infile1 = argv[optind + 1];
	input->infile2 = argv[optind + 2];
	input->data = (unsigned int *) malloc(sizeof(int));

	err = syscall(__NR_xmergesort, (void *) input);
	if (err == 0) {
		if ((input->flags & 0x20) != 0) {
			printf("Number of lines written to out file : %d\n",
			       *input->data);
		}
	} else {
		perror("[sys_call] ");
	}

out:
if (input)
	free(input);
	exit(err);
out_ok: exit(err);
}
