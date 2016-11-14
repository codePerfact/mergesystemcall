#include <linux/linkage.h>
#include <linux/moduleloader.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/fs.h>
#include <linux/namei.h>
#include "xmerge.h"
/*
 * max size of the output buffer, that will store data to be written, temporarily
 */
#define MAX_OUTBUF_SIZE (2*PAGE_SIZE)

/*
 * max size of input buffer, which will be used to read to read big chunk of data
 */
#define MAX_INBUF_SIZE (2*PAGE_SIZE)

/*
 * Flags hex values
 */
#define F_OUTPUT_UNIQ 0x01
#define F_OUTPUT_ALL 0x02
#define F_CASE_INSEN 0x04
#define F_CHECK_SORTED 0x10
#define F_RET_COUNT 0x20

asmlinkage extern long
(*sysptr) (void *arg);


/*
 * Structure to store output data temporarily
 * @buffer : char pointer contain data
 * @currsize : current size of output buffer at any given time
 * @availsize : available empty space in buffer at any given time
 */
typedef struct outbuffer {
	char *buffer;
	unsigned int currsize;
	unsigned int availsize;
} outputbuf;

/*
 * Structure to store input data in big chunks after file read
 * @buffer : char * to contain data
 * @start : start index in buffer
 * @size : current size of input buffer
 */
typedef struct inbuffer {
	char *buffer;
	int start;
	unsigned int size;
} inputbuf;

/*
 *
 * file_line_write : Method to write a line into the file
 * @filp : file pointer to the file in which we want to write
 * @buf : buffer pointer which needs to be written to the file
 * @len : length of the data that needs to be written
 *
 * this method use vfs_write to write to the file
 *
 * Returns number of bytes written to the file, -ve in case of error
 *
 */

static int
file_line_write(struct file *filp, char *buf, int len, outputbuf *outbuf, char *lastout) {
	mm_segment_t oldfs;
	int err = 0;
	int i;

	if (outbuf->availsize < len) {
		oldfs = get_fs();
		set_fs(KERNEL_DS);
		err = vfs_write(filp, outbuf->buffer, outbuf->currsize, &filp->f_pos);
		if (err < 0)
			goto WRITE_OUT;
		set_fs(oldfs);
		outbuf->currsize = 0;
		outbuf->availsize = MAX_OUTBUF_SIZE;
		memset(outbuf->buffer, 0, MAX_OUTBUF_SIZE);
	}
	for (i = 0; i < len; i++)
		outbuf->buffer[(outbuf->currsize)++] = buf[i];
	err = i + 1;
	strcpy(lastout, buf);
	outbuf->availsize = outbuf->availsize - len;
WRITE_OUT:
return err;
}

/*
 * fill_in_buffer : Method used to fill the buffer with file data
 * @filp : file pointer which we need to read
 * @inbuf : buffer which needs to be filled by file data
 *
 * this methon uses vfs_read to read the file
 * returns number of bytes it read, -ve in case of error
 */

static int
fill_in_buffer(struct file *filp, inputbuf *inbuf) {
	int err = 0;
	mm_segment_t oldfs;

	oldfs = get_fs();
	set_fs(KERNEL_DS);
	err = vfs_read(filp, inbuf->buffer + inbuf->size,
	MAX_INBUF_SIZE - inbuf->size, &filp->f_pos);
	if (err < 0)
		goto OUT_FILL;
	set_fs(oldfs);
OUT_FILL:
return err;
}

/*
 * file_line_read : method to read one line from the file/buffer
 * @filp : file pointer which we need to read
 * @buf : buffer in which we will be writing out line
 * @inbuf : temporary structure buffer which is used to cache the data
 *
 * this function tries to read from inbuf if it has some data, else it fills it inbuf again and read next line from it
 *
 * returns number of bytes it read, -ve in case or error
 *
 */
static int
file_line_read(struct file *filp, char *buf, inputbuf *inbuf) {
	int i = 0;
	int err = 0;
	int j = 0;
	char *data = NULL;

	if (inbuf->size == 0) {
		err = fill_in_buffer(filp, inbuf);
		if (err <= 0)
			goto OUT_READ;
		inbuf->start = 0;
		inbuf->size = inbuf->size + err;
	}
	if (inbuf->size > 0) {
		data = (char *) kmalloc(PAGE_SIZE, GFP_KERNEL);
READ_START:
		j = 0;
		i = inbuf->start;
		while (i < (inbuf->start + inbuf->size)) {
			if (inbuf->buffer[i] == '\n') {
				data[j++] = '\n';
				data[j++] = '\0';
				inbuf->start = (i + 1 == MAX_INBUF_SIZE) ? -1 : i + 1;
				inbuf->size = inbuf->size - j + 1;
				strcpy(buf, data);
				err = j - 1;
				goto OUT_READ;
			}
			data[j++] = inbuf->buffer[i];
			i++;
		}
	}

	memcpy(inbuf->buffer, data, j);
	inbuf->size = j;
	inbuf->start = 0;
	err = fill_in_buffer(filp, inbuf);
	if (err < 0) {
		goto OUT_READ;
	} else if (err == 0) {
		inbuf->buffer[j] = '\n';
		inbuf->size = inbuf->size + 1;
	} else {
		inbuf->size = inbuf->size + err;
	}
	goto READ_START;

OUT_READ:
	if (data) {
		kfree(data);
		data = NULL;
	}
	return err;
}

/*
 * this function is used to compare 2 strings
 * @input1 : input line 1
 * @input2 : input line 2
 * @CASE_INSE : this is flag to indicate of comparison needs to be done
 * case insensitive, if value passes is 1 than comparison will be done case
 * insensitive, other wise case sensitive
 * returns a 0 if both strings are same
 * 1 if input1 is greater than input2
 * -1 if input1 is less then input2
 */
static int
strcmputil(char *input1, char *input2, int CASE_INSE) {
	if (CASE_INSE == 1)
		return strcasecmp(input1, input2);
	else
		return strcmp(input1, input2);
}

/*
 * this function will be used to validate the input passed by the user
 * for all possible cases
 * @arg : pointer to the fileinput structure passed by the user
 * return 0 if validation passed
 * return error value if any of the validation failed
 */
static int
validate(fileinput *usrarg) {
	int err = 0;
	struct kstat state;
	int ret;

	/*
	 * check if argument passed are null
	 */
	if (usrarg == NULL) {
		printk(KERN_ERR "invalid argument !!\n");
		err = -EINVAL;
		goto OUT_VALID;
	}
	/*
	 *checking for invalid flag combinations
	 */
	if (((usrarg->flags & F_OUTPUT_UNIQ) == 0)
	    && ((usrarg->flags & F_OUTPUT_ALL) == 0)) { /* ERROR : none of mandatory flag given */
		err = -EINVAL;
		goto OUT_VALID;
	} else if (((usrarg->flags & F_OUTPUT_UNIQ) != 0)
	    && ((usrarg->flags & F_OUTPUT_ALL) != 0)) { /*ERROR : Invalid combination of flags (-a and -u) */
		err = -EINVAL;
		goto OUT_VALID;
	}

	/* check if any of the mandatory parameter in the argument is null */
	if (usrarg->infile1 == NULL || usrarg->infile2 == NULL
	    || usrarg->outfile == NULL) {
		err = -EINVAL;
		goto OUT_VALID;
	} else {
		/*
		 * Now since files are not null checking if they are regular files
		 */
		ret = vfs_stat(usrarg->infile1, &state);
		if (ret) {
			printk(KERN_ERR "input file 1 is not regular\n");
			err = -EINVAL;
			goto OUT_VALID;
		}
		ret = vfs_stat(usrarg->infile2, &state);
		if (ret) {
			printk(KERN_ERR "input file 2 is not regular\n");
			err = -EINVAL;
			goto OUT_VALID;
		}
	}
OUT_VALID:
return err;
}

/*
 *
 * xmergesort : this is main method being used for sorting 2 files given
 * arg : this is fileinput structure pointer passed by user to kernel land
 *
 */

asmlinkage long
xmergesort(void *arg) {
	/* pointer to hold user argument structure */
	fileinput *finput = NULL;

	/* file pointer to hold input file 1 */
	struct file *file_in1 = NULL;

	/* file pointer to hold input file 2 */
	struct file *file_in2 = NULL;

	/* file pointer to hold output file*/
	struct file *file_out = NULL;

	/* temp file pointer to hold processed data*/
	struct file *file_temp = NULL;

	/* buffer to hold lines from input file 1*/
	char *inbuf1 = NULL;

	/*buffer to hold lines from input file 2*/
	char *inbuf2 = NULL;

	/*buffer to hold output lines after merge*/
	outputbuf *outbuf = NULL;

	/*buffer to hold input from file1 in big chunks*/
	inputbuf *inputbuf1 = NULL;

	/*buffer to hold input from file2 in big chunks*/
	inputbuf *inputbuf2 = NULL;

	/*buffer to hold last writte line to output, to keep track if files are sorted*/
	char *lastout = NULL;

	/*err number if occurs*/
	int err = 0;

	/* this variable decides from which file we need to read next
	 * if load == 1 : load input buffer 1 from file 1 with next line
	 * if load == 2 : load input buffer 2 from file 2 with next line
	 * if load == 0 : load both input buffers with next lines from respective files
	 */
	int load = 0;

	/*
	 * this variable will decide which buffer needs to be written to output file
	 * if write == 0 : skip write, don't write any of the buffer
	 * if write == 1 : write input buffer 1 to the output buffer
	 * if write == 2 : write input buffer 2 to the output buffer
	 */
	int write = 0;

	/*
	 * this variable indicate of either of one file is empty or we are done reading it
	 * empty = 0 --> none of the file is finished yet
	 * empty = 1 --> file 1 is finished
	 * empty = 2 --> file 2 is finished
	 */
	int empty = 0;

	/*
	 * this variable will indicate if the comparison needs to be done case sensitive of insensitive
	 * based on the -i flag given by user
	 */
	int INSEN_FLAG = 0;

	/*
	 * This variable will indicate if user has given the option -t
	 * return error if files are not sorted
	 */

	int SORT_FLAG = 0;

	/*
	 * This variable will indicate if user has given the option -u
	 * for uniqe output
	 */
	int UNIQ_FLAG = 0;

	/*
	 * this variable to handle file system while writting buffer to files
	 */
	mm_segment_t oldfs;

	/* this variable is being used to count total number of lines written to output file */
	int i = 0;

	/* finput stores the argument structure passed by user*/
	finput = (fileinput *) kmalloc(sizeof(fileinput), GFP_KERNEL);

	/* check if memory allocation failed*/
	if (finput == NULL) {
		err = -ENOMEM;
		goto OUT;
	}

	/*Copying argument structure from user*/
	err = copy_from_user((void *) finput, arg, sizeof(fileinput));

	/*check if copying arguments failed*/
	if (err != 0) {
		err = -EFAULT;
		goto OUT;
	}

	/*validate basic argument*/
	err = validate(finput);
	if (err != 0) {
		printk(KERN_ERR "basic validation failed!!\n");
		err = -EINVAL;
		goto OUT;
	}

	/*opening input and output files*/
	file_in1 = filp_open(finput->infile1, O_RDONLY, 0);
	file_in2 = filp_open(finput->infile2, O_RDONLY, 0);
	file_temp = filp_open("temp.txt", O_WRONLY | O_CREAT, 0);

	/*setting permissions same as input file*/
	file_temp->f_path.dentry->d_inode->i_mode =
	    file_in1->f_path.dentry->d_inode->i_mode;
	file_out = filp_open(finput->outfile, O_WRONLY | O_CREAT, 0);

	/*setting permissions same as input file*/
	file_out->f_path.dentry->d_inode->i_mode =
	    file_in1->f_path.dentry->d_inode->i_mode;
	if (!file_in1 || !file_in2 || !file_out || !file_temp || IS_ERR(file_in1)
	    || IS_ERR(file_in2) || IS_ERR(file_out) || IS_ERR(file_temp)) {
		printk(KERN_ERR "open FILE ERROR\n");
		err = -EACCES;
		goto OUT;
	}

	/*
	 * checking if any of above files are same
	 */
	if (file_in1->f_inode == file_in2->f_inode) {
		printk(KERN_ERR "file 1 and file 2 are same\n");
		err = -EINVAL;
		goto OUT;
	}

	if (file_in1->f_inode == file_out->f_inode) {
		printk(KERN_ERR "file 1 and output file are same\n");
		err = -EINVAL;
		goto OUT;
	}

	if (file_in2->f_inode == file_out->f_inode) {
		printk(KERN_ERR "file 2 and output file are same\n");
		err = -EINVAL;
		goto OUT;
	}

	/* Input buffer to read from file 1 */
	inbuf1 = (char *) kmalloc(PAGE_SIZE, GFP_KERNEL);

	/*checking for error in assigning buffer memory*/
	if (inbuf1 == NULL) {
		err = -ENOMEM;
		goto OUT;
	}

	/*input buffer to read from file 2*/
	inbuf2 = (char *) kmalloc(PAGE_SIZE, GFP_KERNEL);

	/*checking for error in assigning buffer memory*/
	if (inbuf2 == NULL) {
		err = -ENOMEM;
		goto OUT;
	}

	/*output buffer to store the last line that been written to output*/
	lastout = (char *) kmalloc(PAGE_SIZE, GFP_KERNEL);

	/*checking for error in assigning buffer memory*/
	if (lastout == NULL) {
		err = -ENOMEM;
		goto OUT;
	}

	/*Creating a out buffer of page size bytes to store the merged data temporarily*/
	outbuf = (outputbuf *) kmalloc(sizeof(outputbuf), GFP_KERNEL);
	if (outbuf == NULL) {
		err = -ENOMEM;
		goto OUT;
	}
	outbuf->buffer = (char *) kmalloc(MAX_OUTBUF_SIZE, GFP_KERNEL);
	if (outbuf->buffer == NULL) {
		err = -ENOMEM;
		goto OUT;
	}
	outbuf->currsize = 0;
	outbuf->availsize = MAX_OUTBUF_SIZE;

	inputbuf1 = (inputbuf *) kmalloc(sizeof(inputbuf), GFP_KERNEL);
	if (inputbuf1 == NULL) {
		err = -ENOMEM;
		goto OUT;
	}
	inputbuf1->buffer = (char *) kmalloc(MAX_INBUF_SIZE, GFP_KERNEL);
	if (inputbuf1->buffer == NULL) {
		err = -ENOMEM;
		goto OUT;
	}
	inputbuf1->start = -1; /*-1 indicate buffer is empty*/

	inputbuf1->size = 0;

	inputbuf2 = (inputbuf *) kmalloc(sizeof(inputbuf), GFP_KERNEL);
	if (inputbuf2 == NULL) {
		err = -ENOMEM;
		goto OUT;
	}
	inputbuf2->buffer = (char *) kmalloc(MAX_INBUF_SIZE, GFP_KERNEL);
	if (inputbuf2->buffer == NULL) {
		err = -ENOMEM;
		goto OUT;
	}
	inputbuf2->start = -1; /*-1 indicate buffer in empty*/
	inputbuf2->size = 0;

	/*Reading first line of file 1 in buffer setting empty if file is empty*/
	err = file_line_read(file_in1, inbuf1, inputbuf1);
	if (err <= 0) {
		if (err < 0) {
			err = -EFAULT;
			goto OUT;
		}
		empty = 1;
	}

	/*Reading first line of file 2 in buffer setting empty if file is empty*/
	err = file_line_read(file_in2, inbuf2, inputbuf2);
	if (err <= 0) {
		if (err < 0) {
			err = -EFAULT;
			goto OUT;
		}
		empty = 2;
	}

	/*
	 * checking if -i flag is given, if yes then setting the value
	 */
	if ((finput->flags & F_CASE_INSEN) != 0)
		INSEN_FLAG = 1;

	/*
	 * checking if -t flag is given for sorting, if yes than setting the value
	 */
	if ((finput->flags & F_CHECK_SORTED) != 0)
		SORT_FLAG = 1;

	/*
	 * checking if -u flag is given for unique elements
	 */
	if ((finput->flags & F_OUTPUT_UNIQ) != 0)
		UNIQ_FLAG = 1;

	/*
	 * starting of the while loop for sorting,
	 * this will go on until one of the file is empty
	 */
	while (empty == 0) {

		/*
		 * This while loop has 3 parts
		 * It runs until one of the input file is empty
		 *
		 * PART 1 :
		 * this part set the value of variable "write"  and "load"
		 * (check the variables description near declarations) based on different result of comparisons
		 *
		 * PART 2 :
		 * this part is used to write to the out buffer based of value of "write" variable
		 *
		 * PART 3 :
		 * this is used for filling the buffers with next lines based on variable "load"
		 *
		 */

		/*
		 * PART 1
		 * setting values of "write" and "load" (check their description near declarations)
		 * based of below conditions possible
		 * condition 1.1 : (inbuf1 < inbuf2 && inbuf1 > lastout) --> write = 1, load = 1
		 * condition 1.2 : (inbuf1 < inbuf2 && inbuf1 == lastout) --> write = 1/0 (based on -u flag), load = 1
		 * condition 1.3 : (inbuf1 < inbuf2 && inbuf1 < lastout) --> write = 0(exit code, or continue based on -t flag), load = 1
		 *
		 * condition 2.1 : (inbuf2 < inbuf1 && inbuf2 > lastout) --> write = 2, load = 2
		 * condition 2.2 : (inbuf2 < inbuf1 && inbuf2 == lastout) --> write = 2/0 (based on -u flag), load = 2
		 * condition 2.3 : (inbuf2 < inbuf1 && inbuf2 < lastout) --> write = 0(exit code or continue based on -t flag), load = 2
		 *
		 * condition 3.1 : (inbuf1 == inbuf2 && inbuf1 > lastout) --> write = 1, load = 1
		 * condition 3.2 : (inbuf1 == inbuf2 && inbuf1 == lastout) --> write = 1/0 (based on -u flag) , load = 1/0 (based on -u flag)
		 * condition 3.3 : (inbuf1 == inbuf2 && inbuf1 < lastout) --> write = 0(exit code or continue based on -t flag), load = 0
		 */

		if (strcmputil(inbuf1, inbuf2, INSEN_FLAG) < 0 && strlen(lastout) > 0) { /*Condition 1*/
			if (strcmputil(inbuf1, lastout, INSEN_FLAG) > 0) { /*Condition 1.1*/
				write = 1;
			} else if (strcmputil(inbuf1, lastout, INSEN_FLAG) == 0) { /*Condition 1.2*/
				if (UNIQ_FLAG == 1)
					write = 0;
				 else
					write = 1;
			} else { /*Condition 1.3*/
				if (SORT_FLAG) {
					printk(KERN_ERR "input files are not sorted\n");
					err = -EINVAL;
					goto OUT;
				} else {
					write = 0;
				}
			}
			load = 1;
		} else if (strcmputil(inbuf2, inbuf1, INSEN_FLAG) < 0
		    && strlen(lastout) > 0) { /*Condition 2*/
			if (strcmputil(inbuf2, lastout, INSEN_FLAG) > 0) { /*Condition 2.1*/
				write = 2;
			} else if (strcmputil(inbuf2, lastout, INSEN_FLAG) == 0) { /*Condition 2.2*/
				if (UNIQ_FLAG == 1)
					write = 0;
				 else
					write = 2;
			} else { /*Condition 2.3*/
				if (SORT_FLAG) {
					printk(KERN_ERR "input files are not sorted\n");
					err = -EINVAL;
					goto OUT;
				} else {
					write = 0;
				}
			}
			load = 2;
		} else if (strcmputil(inbuf2, inbuf1, INSEN_FLAG) == 0
		    && strlen(lastout) > 0) { /*Condition 3*/
			if (strcmputil(inbuf1, lastout, INSEN_FLAG) > 0) { /*Condition 3.1*/
				if (strcmputil(inbuf1, inbuf2, 0) < 0) {
					write = 1;
					load = 1;
				} else {
					write = 2;
					load = 2;
				}
			} else if (strcmputil(inbuf1, lastout, INSEN_FLAG) == 0) { /*Condition 3.2*/
				if (UNIQ_FLAG == 1) {
					write = 0;
					load = 0;
				} else {
					write = 1;
					load = 1;
				}
			} else { /*Condition 3.3*/
				if (SORT_FLAG) {
					printk(KERN_ERR "input files are not sorted\n");
					err = -EINVAL;
					goto OUT;
				} else {
					write = 0;
					load = 0;
				}
			}
		} else { /* this is first iteration lastout is not set yet*/
			if (strcmputil(inbuf1, inbuf2, INSEN_FLAG) < 0) {
				write = 1;
				load = 1;
			} else if (strcmputil(inbuf1, inbuf2, INSEN_FLAG) > 0) {
				write = 2;
				load = 2;
			} else if (strcmputil(inbuf1, inbuf2, INSEN_FLAG) == 0) {
				if (strcmputil(inbuf1, inbuf2, 0) < 0) {
					write = 1;
					load = 1;
				} else {
					write = 2;
					load = 2;
				}
			}
		}

		/*
		 * PART 2 :
		 * write but output buffer based of value of variable "write"
		 * write == 1 --> write inbuf 1 to out
		 * write == 2 --> write inbuf 2 to out
		 * write == 0 --> do nothing
		 */
		if (write == 1) {
			err = file_line_write(file_temp, inbuf1, strlen(inbuf1), outbuf, lastout);
			if (err < 0) {
				err = -EFAULT;
				goto OUT;
			}
			++i;
		} else if (write == 2) {
			err = file_line_write(file_temp, inbuf2, strlen(inbuf2), outbuf, lastout);
			if (err < 0) {
				err = -EFAULT;
				goto OUT;
			}
			++i;
		}

		/*
		 * PART 3 :
		 * load inbuf1 or inbuf2 or both based on value of "load" variable
		 * load == 1 --> load inbuf 1
		 * load == 2 --> load inbuf 2
		 * load == 0 --> load both
		 *
		 */
		if (load == 0 || load == 1) {
			err = file_line_read(file_in1, inbuf1, inputbuf1);
			if (err <= 0) {
				if (err < 0) {
					err = -EFAULT;
					goto OUT;
				}
				empty = 1;
				break;
			}
		}

		if (load == 0 || load == 2) {
			err = file_line_read(file_in2, inbuf2, inputbuf2);
			if (err <= 0) {
				if (err < 0) {
					err = -EFAULT;
					goto OUT;
				}
				empty = 2;
				break;
			}
		}

	} /*End of while loop*/

	if (empty == 2) {
		/*
		 *
		 * file 2 finished out, need to write full file 1 to out, including curr value in inbuf1
		 * this do-while loop has 2 parts
		 * PART 1: assign value of write variable based on comparison of values
		 * PART 2 : writing the inbuf1 to out buf accoring to the value of "write" variable
		 *
		 */
		do {
			/*
			 * PART 1
			 * Assigning value of write variable based of comparison of inbuf1 and lastout
			 */
			if ((strlen(lastout) > 0)
			    && (strcmputil(inbuf1, lastout, INSEN_FLAG) > 0)) {
				write = 1;
			} else if ((strlen(lastout) > 0)
			    && (strcmputil(inbuf1, lastout, INSEN_FLAG) < 0)) {
				if (SORT_FLAG) {
					printk(KERN_ERR "input files are not sorted\n");
					err = -EINVAL;
					goto OUT;
				} else {
					write = 0;
				}
			} else if ((strlen(lastout) > 0)
			    && (strcmputil(inbuf1, lastout, INSEN_FLAG) == 0)) {
				if (UNIQ_FLAG == 1)
					write = 0;
				 else
					write = 1;
			} else { /*lastout is not set yet*/
				write = 1;
			}

			/*PART 2*/

			if (write == 1) {
				err = file_line_write(file_temp, inbuf1, strlen(inbuf1), outbuf, lastout);
				if (err < 0) {
					err = -EFAULT;
					goto OUT;
				}
				++i;
			}
		} while ((err = file_line_read(file_in1, inbuf1, inputbuf1)) > 0);
		if (err < 0) {
			err = -EFAULT;
			goto OUT;
		}
	}

	/*
	 * Writing remaining lines from file 1 to out buffer if left
	 * also checking if remaining part of the file is sorted if
	 * -t option given
	 */
	if (empty == 1) {

		/*
		 * file 1 finished out, need to write full file 2 to out, including curr value in inbuf2
		 * this do-while loop has 2 parts
		 * PART 1: assign value of write variable based on comparison of values
		 * PART 2 : writing the inbuf2 to out buf accoring to the value of "write" variable
		 *
		 */
		do {
			/*
			 * PART 1
			 * Assigning value to "write" variable accordingly
			 *
			 */

			if ((strlen(lastout) > 0)
			    && (strcmputil(inbuf2, lastout, INSEN_FLAG) > 0)) {
				write = 2;
			} else if ((strlen(lastout) > 0)
			    && (strcmputil(inbuf2, lastout, INSEN_FLAG) < 0)) {
				if (SORT_FLAG) {
					printk(KERN_ERR "input files are not sorted\n");
					err = -EINVAL;
					goto OUT;
				} else {
					write = 0;
				}
			} else if ((strlen(lastout) > 0)
			    && (strcmputil(inbuf2, lastout, INSEN_FLAG) == 0)) {
				if (UNIQ_FLAG == 1)
					write = 0;
				else
					write = 2;
			} else {
				write = 2;
			}

			/*PART 2*/
			if (write == 2) {
				err = file_line_write(file_temp, inbuf2, strlen(inbuf2), outbuf, lastout);
				if (err < 0) {
					err = -EFAULT;
					goto OUT;
				}
				++i;
			}
		} while ((err = file_line_read(file_in2, inbuf2, inputbuf2)) > 0);
		if (err < 0) {
			err = -EFAULT;
			goto OUT;
		}
	}

	/*flushing rest of the out buffer to file*/
	oldfs = get_fs();
	set_fs(KERNEL_DS);
	vfs_write(file_temp, outbuf->buffer, outbuf->currsize, &file_temp->f_pos);
	set_fs(oldfs);

	/*copying the number of lines written to output file*/
	err = copy_to_user(finput->data, &i, 2);
	if (err != 0) {
		printk(KERN_ERR "copy to user failed\n");
		err = -EFAULT;
		goto OUT;
	}

	/*Renaming the temp file to given output file*/

	lock_rename(file_out->f_path.dentry->d_parent, file_temp->f_path.dentry->d_parent);

	vfs_rename(file_temp->f_path.dentry->d_parent->d_inode, file_temp->f_path.dentry, file_out->f_path.dentry->d_parent->d_inode, file_out->f_path.dentry, NULL, RENAME_NOREPLACE);

	unlock_rename(file_out->f_path.dentry->d_parent, file_temp->f_path.dentry->d_parent);

OUT: if (inbuf1) {
		kfree(inbuf1);
		inbuf1 = NULL;
	}
	if (inbuf2) {
		kfree(inbuf2);
		inbuf2 = NULL;
	}
	if (outbuf && outbuf->buffer) {
		kfree(outbuf->buffer);
		outbuf = NULL;
	}
	if (lastout) {
		kfree(lastout);
		lastout = NULL;
	}
	if (file_in1)
		filp_close(file_in1, NULL);
	if (file_in2)
		filp_close(file_in2, NULL);
	if (file_out)
		filp_close(file_out, NULL);
	if (finput) {
		kfree(finput);
		finput = NULL;
	}
	return err;
}

/*Entry Function of xmergesort module*/
static int __init init_sys_xmergesort(void)
{
	printk(KERN_INFO "installed new sys_xmergesort module\n");
	if (sysptr == NULL)
	sysptr = xmergesort;
	return 0;
}

/*Exit function of xmergesort module*/
static void __exit exit_sys_xmergesort(void)
{
	if (sysptr != NULL)
	sysptr = NULL;
	printk(KERN_INFO "removed sys_xmergesort module\n");
}

module_init(init_sys_xmergesort);
module_exit(exit_sys_xmergesort);
MODULE_LICENSE("GPL");
