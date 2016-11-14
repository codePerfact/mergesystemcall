
/*
 *
 * Structure to take input from userland to kernel land
 * @infile1 : input file 1 for sorting
 * @infile2 : input file 2 for sorting
 * @outfile : file path in which output needs to be written
 * @flags : options given by user for sorting
 * @data : pointer to int * where line count is stored if requested by user
 *
 */
typedef struct input {
	char *infile1;
	char *infile2;
	char *outfile;
	unsigned int flags;
	unsigned int *data;
} fileinput;
