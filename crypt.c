#include <linux/linkage.h>
#include <linux/moduleloader.h>
#include <linux/fs.h>
#include <stdbool.h>
#include <asm/uaccess.h>
#include <linux/slab.h>
#include <linux/crypto.h>
#include <linux/scatterlist.h>
#include <linux/cryptohash.h>
#include <linux/namei.h>
#include <linux/stat.h>
#define COMMENT 0

#define __NR_xcrypt 359
#define MY_BUFFER_SIZE 4096
#define MAX_FILE_LENGTH 1024
#define MD5_KEY_LENGTH 16
#define SHA_KEY_LENGTH 20

struct input
{

char *infile;
char *outfile;
unsigned char *keybuf;
unsigned int keylen;
int flags;
};

asmlinkage extern long (*sysptr)(void *arg);



void sha(unsigned char *hash, char *plaintext)
{
	struct scatterlist sg;
    struct crypto_hash *tfm;
    struct hash_desc desc;

    memset(hash, 0x00,SHA_KEY_LENGTH);

    tfm = crypto_alloc_hash("sha1", 0, CRYPTO_ALG_ASYNC);

    desc.tfm = tfm;
    desc.flags = 0;

    sg_init_one(&sg, plaintext, 10);
    crypto_hash_init(&desc);

    crypto_hash_update(&desc, &sg, 10);
    crypto_hash_final(&desc, hash);
	crypto_free_hash(tfm);

}

int crypt(void *rbuff,void *wbuff,unsigned int bytes,struct input *in)
{
	struct crypto_blkcipher *tfm=NULL;
    struct blkcipher_desc desc;
	struct scatterlist *src=NULL;
    struct scatterlist *dst=NULL;
	unsigned char *key=in->keybuf;
	int ret=0;
	char *iv ="\x12\x34\x56\x78\x90\xab\xcd\xef\x12\x34\x56\x78\x90\xab\xcd\xef";
	unsigned int ivsize = 0;
	char *algo = "ctr(aes)";
	
	src = kmalloc(sizeof(struct scatterlist), __GFP_ZERO|GFP_KERNEL);
    if (!src) {
        printk("[crypt]: failed to alloc src\n");     
        ret= -1;
		goto out;
    }
    dst = kmalloc(sizeof(struct scatterlist), __GFP_ZERO|GFP_KERNEL);
    if (!dst) {
        printk("[crypt]: failed to alloc dst\n"); 
        ret= -1;
		goto out;
    }
	
	tfm = crypto_alloc_blkcipher(algo, 0, 0);
	
	 if (IS_ERR(tfm)) {
        printk("[crypt]: failed to load transform for : %ld\n",PTR_ERR(tfm));
		ret=-1;
        goto out;
    }
    desc.tfm = tfm;
    desc.flags = 0;
    
    ret = crypto_blkcipher_setkey(tfm, key, MD5_KEY_LENGTH);
	 if (ret) {
			printk("[crypt]: setkey() failed flagss=%x\n",
            crypto_blkcipher_get_flags(tfm));
			goto out;
    }
	
	ivsize = crypto_blkcipher_ivsize(tfm);
	if (ivsize) {
		if (ivsize != strlen(iv))
			printk("[crypt]: IV length differs from expected length\n");
			crypto_blkcipher_set_iv(tfm, iv, ivsize);
	}
	
	sg_init_table(src, 1);
	sg_init_table(dst, 1);
		
	sg_set_buf(src, rbuff, bytes);
	sg_set_buf(dst, wbuff, bytes);
	
	if(in->flags){
		ret=crypto_blkcipher_encrypt(&desc,dst,src,bytes);
		if(ret<0)
		{
			printk("[crypt]: crypto_blkcipher_encrypt FAILED");
			goto out;
		}
	}
	else{
		ret=crypto_blkcipher_decrypt(&desc,dst,src,bytes);
		if(ret<0)
		{
			printk("[crypt]: crypto_blkcipher_decrypt FAILED");
			goto out;
		}
	}	
			
out:
    if(dst)
		kfree(dst);
	if(src)
		kfree(src);
	if(tfm)
		crypto_free_blkcipher(tfm);

	return ret;
}



int rwfile(struct file *filp1, struct file *filp2, struct input *in)
{
    void *rbuff=NULL, *wbuff=NULL;
	unsigned char *preamble=NULL,*new_preamble=NULL,*key=in->keybuf;
	int err=0;
	unsigned int bytes=MY_BUFFER_SIZE;
	loff_t rpos = 0, wpos=0;
	mm_segment_t oldfs;
	struct file *filp3=NULL;
	
    filp3 = filp_open("a.tmp", MAY_WRITE|O_CREAT|O_TRUNC, 0);
	if(!filp3){
		printk("[rwfile] : temporary file open FAILED");
		err = -1;
		goto out;
	}
	
	rbuff = kmalloc(MY_BUFFER_SIZE,GFP_KERNEL);
	if (!rbuff) {
		printk("[rwfile] : kmalloc FAILED");
    	err = -ENOMEM;
		goto out;
  	}
	wbuff = kmalloc(MY_BUFFER_SIZE,GFP_KERNEL);
	if (!wbuff) {
		printk("[rwfile] : kmalloc FAILED");
    	err = -ENOMEM;
		goto out;
  	}
	preamble = kmalloc(SHA_KEY_LENGTH,GFP_KERNEL);
	if (!preamble) {
		printk("[rwfile] : kmalloc FAILED");
    	err = -ENOMEM;
		goto out;
  	}
	new_preamble = kmalloc(SHA_KEY_LENGTH,GFP_KERNEL);
	if (!new_preamble) {
		printk("[rwfile] : kmalloc FAILED");
    	err = -ENOMEM;
		goto out;
  	}
	
	sha(preamble,key);
	
	if(in->flags)
	{
		//encryption, put the preamble into write file.
		oldfs = get_fs();
		set_fs(KERNEL_DS);
		err=vfs_write(filp3, preamble, SHA_KEY_LENGTH, &wpos);
		if(err<0)
		{
			printk("[rwfile] : vfs_write FAILED");
			goto out;
		}
		set_fs(oldfs);
			
	}
	else
	{
		//decryption, extract new_preamble from read file, compare, if yes, then go ahead else fail here.
		oldfs = get_fs();
		set_fs(KERNEL_DS);
		err=vfs_read(filp1, new_preamble, SHA_KEY_LENGTH, &rpos);
		if(err<0)
		{
			printk("[rwfile] : vfs_read FAILED");
			goto out;
		}
		set_fs(oldfs);
	
		if(0!=memcmp(preamble,new_preamble,SHA_KEY_LENGTH))
		{
			printk("\nwrong decrypt key, DECRYTION FAILED \n");
			err=-1;
			goto out;
		}
	}
	
	 do{	
	oldfs = get_fs();
    set_fs(KERNEL_DS);
	bytes=vfs_read(filp1, rbuff, PAGE_SIZE, &rpos);
	set_fs(oldfs);

	if(bytes<0)
	{
		printk("[rwfile]: vfs_read FAILED\n");
		err=bytes;
		goto out;
	}
	
	err=crypt(rbuff,wbuff,bytes,in);
	if(err<0)
	{
		printk("[rwfile]: crypt FAILED\n");
		goto out;
	}
	
	oldfs = get_fs();
    set_fs(KERNEL_DS);
	err=vfs_write(filp3, wbuff, bytes, &wpos);
	if(bytes<0)
	{
		printk("[rwfile]: vfs_write FAILED\n");
		goto out;
	}
	set_fs(oldfs);
	
	}while(bytes>0);
	
	
	memset(rbuff,0,MY_BUFFER_SIZE);
	memset(wbuff,0,MY_BUFFER_SIZE);
	rpos=0;
	wpos=0;
	
	
	 do{
		 
	oldfs = get_fs();
    set_fs(KERNEL_DS);
	
	bytes=vfs_read(filp3, rbuff, PAGE_SIZE, &rpos);
	
	if(bytes<0)
	{
		printk("[rwfile]: vfs_read FAILED\n");
		goto out;
	}
	
	
	err=vfs_write(filp2, rbuff, bytes, &wpos);
	
	if(bytes<0)
	{
		printk("[rwfile]: vfs_write FAILED\n");
		goto out;
	}
	set_fs(oldfs);
	
	}while(bytes>0);
	
	
	lock_rename(filp3->f_path.dentry->d_parent,filp2->f_path.dentry->d_parent);
    
    vfs_unlink(filp3->f_path.dentry->d_parent->d_inode,filp3->f_path.dentry, NULL);
	
    unlock_rename(filp3->f_path.dentry->d_parent,filp2->f_path.dentry->d_parent);
	
	
	
out:
	if(filp3)
		filp_close(filp3, NULL);
	if(new_preamble)
		kfree(new_preamble);
	if(preamble)
		kfree(preamble);
	if(wbuff)
		kfree(wbuff);
	if(rbuff)
		kfree(rbuff);
	

	return err;
}

asmlinkage long xcrypt(void *arg)
{
	struct kstat sb;
	struct input *in=NULL;
	struct input *u_in=NULL;
	struct file *filp1=NULL, *filp2=NULL;
    int err=0;
	in = kmalloc(sizeof(struct input),GFP_KERNEL);
	if (!in) {
    	err = -1;
		printk("\n[xcrypt]: KMALLOC FAILED\n");
  	}

	u_in = (struct input *)arg;
	
	err = copy_from_user(in, arg, sizeof(struct input));
	if (err!=0) {
		printk("[xcrypt]: copy_from_user FAILED\n");
    	goto out_ok;
  	}
	
	in->keybuf=kmalloc(MD5_KEY_LENGTH, GFP_KERNEL);
	if (!in->keybuf) {
				printk("\n[xcrypt]: KMALLOC FAILED\n");
    	err = -1;
  	}
	
	err = copy_from_user(in->keybuf, u_in->keybuf, in->keylen);
	if (err!=0) {
		printk("[xcrypt]: copy_from_user FAILED\n");
    	goto out_ok;
  	}
	
	vfs_stat(u_in->infile, &sb);
    if ((sb.mode & S_IFMT) != S_IFREG) {
		printk("[xcrypt]: Infile is not a regular file\n");
         err=-1;
		 goto out_ok;
    }
	
	vfs_stat(u_in->outfile, &sb);
    if ((sb.mode & S_IFMT) != S_IFREG) {
		printk("[xcrypt]: Outfile is not a regular file\n");
         err=-1;
		 goto out_ok;
    }
	
    filp1 = filp_open(in->infile, MAY_READ, 0);
    if (!filp1 || IS_ERR(filp1)) {
	printk("[xcrypt]: wrapfs_read_file err %d\n", (int) PTR_ERR(filp1));
	err=PTR_ERR(filp1);
	goto out_ok;
    }
	
    filp2 = filp_open(in->outfile, MAY_WRITE|O_CREAT, 0);
    if (!filp2 || IS_ERR(filp2)) {
	printk("[xcrypt]: wrapfs_write_file err %d\n", (int) PTR_ERR(filp2));
	err=PTR_ERR(filp2);
	goto out_ok;
    }

	if(filp1->f_inode==filp2->f_inode)
	{
		printk("[xcrypt]: The two files are same\n");
		err=-1;
		goto out_ok;
	}
	err=rwfile(filp1,filp2,in);
	if(err!=0)
	{
		printk("[xcrypt]: File read write operation failed\n");
		goto out_ok;
	}   
    	
 

 	out_ok:
	if(filp1)
		filp_close(filp1, NULL);
	if(filp2)
		filp_close(filp2, NULL);
	if(in->keybuf)
		kfree(in->keybuf);
	if(in)
		kfree(in);
	
  	 return err;


}

static int __init init_sys_xcrypt(void)
{
	printk("installed new sys_xcrypt module\n");
	if (sysptr == NULL)
		sysptr = xcrypt;
	return 0;
}
static void  __exit exit_sys_xcrypt(void)
{
	if (sysptr != NULL)
		sysptr = NULL;
	printk("removed sys_xcrypt module\n");
}
module_init(init_sys_xcrypt);
module_exit(exit_sys_xcrypt);
MODULE_LICENSE("GPL");