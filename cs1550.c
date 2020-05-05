/*
	FUSE: Filesystem in Userspace
	Copyright (C) 2001-2007  Miklos Szeredi <miklos@szeredi.hu>

	(C) 2020 Steven Montalbano <smm285@pitt.edu>

	USAGE: make; ./cs1550 -d /TESTMOUNT
	NOTE: -d runs FUSE in the foreground + displays all stderr prints,
				use a second terminal window to interact and debug when using -d
	DEBUG: gdb .libs/lt-cs1550;
				(gdb) r -d /TESTMOUNT

	This program can be distributed under the terms of the GNU GPL.
	See the file COPYING.

	Error Codes: https://www.gnu.org/software/libc/manual/html_node/Error-Codes.html
*/

#define	FUSE_USE_VERSION 26

#include <fuse.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>

// Outline disk and block structure
#define DISK_SIZE 5242880 // 5 MB = 5242880 Bytes
#define	BLOCK_SIZE 512		// Each block is 512 Bytes
#define DISK_BLOCKS 10240	// DISK_SIZE / BLOCK_SIZE = 10240 blocks
#define BIT_MAP_SIZE 1536 // 512*3 -> last 3 blocks of the file
#define	MAX_DATA_IN_BLOCK (BLOCK_SIZE)
#define BIT_MAP_LOCATION (DISK_SIZE - (BLOCK_SIZE * 3))

// Outline file naming conventions (8.3)
#define	MAX_FILENAME 8
#define	MAX_EXTENSION 3

// Outline directiry and index block structure
#define MAX_DIRS_IN_ROOT (BLOCK_SIZE - sizeof(int)) / ((MAX_FILENAME + 1) + sizeof(long))
#define MAX_FILES_IN_DIR (BLOCK_SIZE - sizeof(int)) / ((MAX_FILENAME + 1) + (MAX_EXTENSION + 1) + sizeof(size_t) + sizeof(long))
#define	MAX_ENTRIES_IN_INDEX_BLOCK (BLOCK_SIZE/sizeof(long))

#define streq(a, b) (strcmp((a), (b)) == 0) 																	// Overloaded strcmp Macro
#define strneq(a, b, l) (strncmp((a), (b), (l)) == 0) 												// Overloaded strncmp Macro
#define SEEK_BITMAP {rewind(disk); fseek(disk, BIT_MAP_LOCATION, SEEK_SET);}	// Move file pointer to bitmap
#define SEEK_ROOT		{rewind(disk); fseek(disk, 0, SEEK_SET);}									// Move file pointer to beginning of disk
#define ceiling(a,b) ((a) / (b)) + (((a) % (b)) != 0)													// ceil(a, b) function, issues when linking <math.h>
#define MAX(a,b) (((a)>(b))?(a):(b))																					// max(a, b) function, 	^		^

typedef unsigned char byte;										// An unsigned char is exactly 1 byte (8 bits) in size
static byte bitmap[BIT_MAP_SIZE]; 						// a bit will represent each disk block, 1 = used block, 0 = free block
static const char* DISK_FILE_PATH = ".disk";	// The .disk path
static FILE* disk;														// Global pointer to the open disk file

//Function prototypes: TODO fix error: expected ‘=’, ‘,’, ‘;’, ‘asm’ or ‘__attribute__’ before ‘*’ token
// static int write_disk_block(long block_num, void* data);
// static void* get_block(long block_num);
// static directory_entry* get_dir(long block_num);
// static root_directory* get_root_dir(void);
// static int is_bit_set(byte data, int p);
// static void initBitmap(void);

// Struct to represent the root directory, NOTE root can only contain other dirs
//The attribute packed means to not align these things
typedef struct cs1550_root_directory
{
	int nDirectories;	//How many subdirectories are in the root
										//Needs to be less than MAX_DIRS_IN_ROOT
	struct cs1550_directory
	{
		char dname[MAX_FILENAME + 1];	//directory name (plus space for nul)
		long nStartBlock;							//where the directory block is on disk
	} __attribute__((packed)) directories[MAX_DIRS_IN_ROOT];	//There is an array of these

	//This is some space to get this to be exactly the size of the disk block.
	//Don't use it for anything.
	char padding[BLOCK_SIZE - MAX_DIRS_IN_ROOT * sizeof(struct cs1550_directory) - sizeof(int)];
} root_directory;

// Struct to represent sub directories under root
typedef struct cs1550_directory_entry
{
	int nFiles;	// How many files are in this directory.
							// Needs to be less than MAX_FILES_IN_DIR
	struct cs1550_file_directory
	{
		char fname[MAX_FILENAME + 1];	// filename (plus space for nul)
		char fext[MAX_EXTENSION + 1];	// extension (plus space for nul)
		size_t fsize;					// file size
		long nIndexBlock;			// where the index block is on disk
	} __attribute__((packed)) files[MAX_FILES_IN_DIR];	//There is an array of these

	//This is some space to get this to be exactly the size of the disk block.
	//Don't use it for anything.
	char padding[BLOCK_SIZE - MAX_FILES_IN_DIR * sizeof(struct cs1550_file_directory) - sizeof(int)];
} directory_entry;

// Struct to represent an index node for index allocation
typedef struct cs1550_index_block
{
    //All the space in the index block can be used for index entries.
		// Each index entry is a data block number.
		long entries[MAX_ENTRIES_IN_INDEX_BLOCK];
} index_block;

// Struct to represent a block of data on the disk, holds file contents
typedef struct cs1550_disk_block
{
	//All of the space in the block can be used for actual data
	//storage.
	char data[MAX_DATA_IN_BLOCK];
} disk_block;

// Helper Functions:

// Represents a 1 in each of the 8 fields of a unsigned byte from MSB -> LSB
static const byte mask[] = {128, 64, 32, 16, 8, 4, 2, 1};

// Marks the bit representing the root directory and bitmap as allocated
// root = first bit of map, bitmap = last 3 bits of the map
static void initBitmap(void){
	// fprintf(stderr, "*** initBitMap function call:\n");
	SEEK_BITMAP
	fread(bitmap, BIT_MAP_SIZE, 1, disk);

	bitmap[0] |= mask[0];	// Init cs1550_root_directory
	bitmap[BIT_MAP_SIZE-1] |= mask[5]; //Init the last 3 blocks for the bitmap itself
	bitmap[BIT_MAP_SIZE-1] |= mask[6];
	bitmap[BIT_MAP_SIZE-1] |= mask[7];
}

//Check the bit at position p in the byte data by a logical AND with the mask[]
//returns 1 of the bit is 1, 0 if the bit is 0
static int is_bit_set(byte data, int p) {
	return ((data & mask[p]) != 0);
}

// Set the bit representing the block_num to 1 using logical OR with the mask[]
static int set_bit(long block_num){

	fprintf(stderr, "*** set_bit function call: setting block %ld\n", block_num);

	if(block_num > DISK_BLOCKS){
		fprintf(stderr, "*** set_bit: requested block %ld is out of .disk bounds\n", block_num);
		return -1;
	}

	long byte_num = (block_num / 8);	// Find the byte that contains the bit for block_num
	long bit_num  = (block_num % 8);	// Find the bit position inside the byte for block_num

	bitmap[byte_num] |= mask[bit_num];	// Set the bit to 1 to mark allocated

	return 0;
}

// Iterate through the bitmap bit by bit to return the block
// index of the next free disk block
static long find_free_block(void) {
	printf("*** find_free_block function call\n");

	if(is_bit_set(bitmap[BIT_MAP_SIZE-1], 7) == 0){
		printf("*** find_free_block: bitmap is uninit: calling initBitmap()\n");
		initBitmap();
	}

	int found = 0, byte_num, bit_num;		//Byte number and bit number
	for(byte_num = 0; byte_num < BIT_MAP_SIZE; byte_num++){
		for(bit_num = 0; bit_num < 8; bit_num++) {	// Search inside byte_num from most significant bit_num to least
			if(!is_bit_set(bitmap[byte_num], bit_num)){
				found = 1;
				break;
			}
		}
		if(found) break;
	}

	if(!found)					// If for loops exited without breaking, no free blocks found,
		return -ENOSPC;		// return out of disk space

	long free_block = (8 * byte_num) + bit_num;	// Convert the byte and bit numbers into the block index

	printf("*** find_free_block: free block index %ld\n", free_block);

	return free_block;
}

// Save a disk block back to the .disk file
static int write_disk_block(long block_num, void* block){

	fprintf(stderr, "*** write_disk_block function call: writing @ block %ld\n", block_num);

	if(block_num > DISK_BLOCKS){
		fprintf(stderr, "*** write_disk_block: requested block %ld is out of .disk bounds\n", block_num);
		return -1;
	}

	SEEK_ROOT
	if(fseek(disk, (block_num * BLOCK_SIZE), SEEK_SET) != 0){
		fprintf(stderr, "*** write_disk_block: failed to seek to block %ld\n", block_num);
		return -1;
	}

	if(fwrite(block, BLOCK_SIZE, 1, disk) != 1){
		fprintf(stderr, "*** write_disk_block: failed to write block to disk\n");
		return -1;
	}

	return 0;
}

// Seek through the disk to the specified block_num and return the data as void*,
// allows for code reuse from helper functions below
static void* get_block(long block_num){

	if(block_num > DISK_BLOCKS){
		fprintf(stderr, "**get_block: requested block %ld is out of .disk bounds\n", block_num);
		return NULL;
	}

	if(fseek(disk, block_num * BLOCK_SIZE, SEEK_SET) != 0){
		fprintf(stderr, "**get_block: could not seek to block %ld\n", block_num);
		return NULL;
	}

	void* block = malloc(BLOCK_SIZE);

	if(fread(block, BLOCK_SIZE, 1, disk) != 1){
		fprintf(stderr, "**get_block: requested block %ld could not be read\n", block_num);
		free(block);
		return NULL;
	}
	return block;
}

static index_block* get_inode(long block_num){
	return (index_block*) get_block(block_num);
}

static disk_block* get_disk_block(long block_num){
	return (disk_block*) get_block(block_num);
}

static directory_entry* get_dir(long block_num){
	return (directory_entry*) get_block(block_num);
}

static root_directory* get_root_dir(void){
	return (root_directory*) get_block(0);
}

// End Helper Functions


// File System implementations

/*
 * Called whenever the system wants to know the file attributes, including
 * simply whether the file exists or not.
 *
 *	ex: ls -al /TESTMOUNT
 *
 * man -s 2 stat will show the fields of a stat structure
 */
 static int cs1550_getattr(const char *path, struct stat *stbuf)
 {

   	int res = 0;
   	// Holds the dir, file, and extension
     // char dir[2*MAX_FILENAME + 1];		// The directory name
     // char fname[2*MAX_FILENAME + 1];	// The file name			//NOTE: made bigger to pass gradescope
     // char fext[2*MAX_EXTENSION + 1];	// The file extension

   	root_directory*  root = NULL;
   	directory_entry* cur_dir = NULL;

   	memset(stbuf, 0, sizeof(struct stat));

   	printf("*** getattr function call\n");
     // Check if the path is the root directory
   	if (strcmp(path, "/") == 0) {
   		stbuf->st_mode = S_IFDIR | 0755;
   		stbuf->st_nlink = 2;
			res = 0;
   		printf("*** getattr: root dir\n");
   	} else {
      printf("*** getattr: sub dir\n");
   		printf("*** Path %s\n", path);

			char* dir = calloc(MAX_FILENAME + 1, 1);
			char* fname = calloc(MAX_FILENAME + 1, 1);
			char* fext = calloc(MAX_EXTENSION + 1, 1);

     // Returns the number of tokens parsed, if 0 then format error: dir cant exist
     int tokens = sscanf(path, "/%[^/]/%[^.].%s", dir, fname, fext);

     if(strlen(fname) > MAX_FILENAME || strlen(fext) > MAX_EXTENSION)
 			return -ENAMETOOLONG;

   		if(tokens < 1)
   			return -ENOENT;		// No such file or directory
         // Debug prints
   	 	printf("directory:%s\n", dir);
   		printf("filename:%s\n", fname);
   		printf("extension:%s\n", fext);

   		root = get_root_dir();

	     if(root == NULL)
	       fprintf(stderr, "*** getattr: Root is null!\n");
	     else
	       fprintf(stderr, "*** getattr: Root is NOT null!\n");

	     int i;
	     for(i = 0; i < root->nDirectories; i++){
	       if(strneq(dir, root->directories[i].dname, MAX_FILENAME))
	         break;
	     }

       if(i == root->nDirectories){	// If at last dir without a match:
   			res = -ENOENT;							// return not found
   			printf("*** getattr: i == nDirectories\n");
   		} else {
   			// Directory found
   			if(tokens == 1) {	// 1 token means only a dir name was parsed
   				stbuf->st_mode = S_IFDIR | 0755;
   				stbuf->st_nlink = 2;
   				res = 0; //no error
   				printf("*** getattr: sub dir found!\n");
   			} else {	// more than 1 token means a file and ext were parsed,
   								// check for the file name and ext in the directory
   				printf("*** getattr: looking for file\n");

   				// Set return to not fount by default
   				res = -ENOENT;

   				// Load the sub directory to find the file
   				cur_dir = get_dir(root->directories[i].nStartBlock);

   				// Search cur_dir for the file
   				for(i = 0; i < cur_dir->nFiles; i++){
   					if(strneq(fname, cur_dir->files[i].fname, MAX_FILENAME) &&
   					   strneq(fext, cur_dir->files[i].fext, MAX_EXTENSION)) {
   							 // File matching fname and extension found
   							 stbuf->st_mode = S_IFREG | 0666;
   							 stbuf->st_nlink = 1; //file links
   							 stbuf->st_size = cur_dir->files[i].fsize; //file size
   							 res = 0; // no error
   							 printf("*** getattr: file found!\n");
   							 break;
   						 }
   				}	// End for loop
   			}	// End file search
   		}	// End directory found

   		if(root != NULL) free(root);
   		if(cur_dir != NULL) free(cur_dir);

			free(dir);
			free(fname);
			free(fext);
		}
 	return res;
 }

/*
 * Called whenever the contents of a directory are desired. Could be from an 'ls'
 * or could even be when a user hits TAB to do autocompletion
 */
static int cs1550_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
			 off_t offset, struct fuse_file_info *fi)
{
	fprintf(stderr, "*** readdir function call\n");
	//Since we're building with -Wall (all warnings reported) we need
	//to "use" every parameter, so let's just cast them to void to
	//satisfy the compiler
	(void) offset;
	(void) fi;

	int res;
	root_directory* root = NULL;
	directory_entry* sub_dir = NULL;
	char dir[MAX_FILENAME + 1];		// The directory name
	char fname[MAX_FILENAME + 1];	// The file name
	char fext[MAX_EXTENSION + 1];	// The file extension
	// Init vars to 0
	memset(dir, 0, MAX_FILENAME + 1);		//TODO change to calloc to follow convention
	memset(fname, 0, MAX_FILENAME + 1);
	memset(fext, 0, MAX_EXTENSION + 1);

	sscanf(path, "/%[^/]/%[^.].%s", dir, fname, fext);

	root = get_root_dir();

	if(streq(path, "/")){	// root dir requested, list all sub dirs
		printf("*** readdir: root requested, sub_dirs:\n");

		filler(buf, ".", NULL, 0);	// Self, this directory
		filler(buf, "..", NULL, 0);	// Parent directory

		int i;
		for(i = 0; i < root->nDirectories; i++){
			filler(buf, root->directories[i].dname, NULL, 0);	// Display the dir name
		}
		res = 0;

	} else {	// sub dir requested
		printf("*** readdir: sub_dir %s requested, contens:\n", dir);

		filler(buf, ".", NULL, 0);	// Self, this directory
		filler(buf, "..", NULL, 0);	// Parent directory

		if(root == NULL)
			fprintf(stderr, "*** readdir: Root is null!\n");
		else
			fprintf(stderr, "*** readdir: Root is NOT null!\n");

		//Search for the sub_dir under root
		int i;
		for(i = 0; i < root->nDirectories; i++){
			if(strneq(dir, root->directories[i].dname, MAX_FILENAME)){
				printf("*** readdir: sub dir found!\n");
				sub_dir = get_dir(root->directories[i].nStartBlock);
				break;
			}
		}

		if(!sub_dir) {	// If sub_dir is empty, return no such directory
			printf("*** readdir: sub dir not found, returning error!\n");
			return -ENOENT;
		}

		char full_name[MAX_FILENAME + MAX_EXTENSION + 2];	// leave room for the null terminator char
		//List sub dir contents
		for(i = 0; i < sub_dir->nFiles; i++){
			strncpy(full_name, sub_dir->files[i].fname, MAX_FILENAME + 1);
			strcat(full_name, ".");
			strncat(full_name, sub_dir->files[i].fext, MAX_EXTENSION + 1);
			filler(buf, full_name, NULL, 0);
		}

		res = 0;
	}

	if(root != NULL) free(root);
	if(sub_dir != NULL) free(sub_dir);

	return res;
}

/*
 * Creates a directory. We can ignore mode since we're not dealing with
 * permissions, as long as getattr returns appropriate ones for us.
 */
static int cs1550_mkdir(const char *path, mode_t mode)
{
	printf("*** mkdir function call\n");

	(void) path;
	(void) mode;
	int res = 0, i;
	root_directory* root = NULL;
	char dir[MAX_FILENAME + 1];		// The directory name
	char fname[MAX_FILENAME + 1];	// The file name
	char fext[MAX_EXTENSION + 1];	// The file extension
	// Init vars to 0
	memset(dir, 0, MAX_FILENAME + 1);
	memset(fname, 0, MAX_FILENAME + 1);
	memset(fext, 0, MAX_EXTENSION + 1);

	int tokens = sscanf(path, "/%[^/]/%[^.].%s", dir, fname, fext);

	if(tokens > 1)	// sub directories can only exist under root
		return -EPERM;

	if(strlen(dir) > MAX_FILENAME)
		return -ENAMETOOLONG;

	root = get_root_dir();

	if(!root){
		fprintf(stderr, "*** mkdir: error reading the root dir\n");
		return -ENOENT;
	}

	if(root->nDirectories == MAX_DIRS_IN_ROOT){ // root is full, return error
		printf("*** mkdir: root is full\n");
		return -ENOSPC;
	}

	for(i = 0; i < root->nDirectories; i++){
		if(strneq(dir, root->directories[i].dname, MAX_FILENAME)){	// Dir already exists
			printf("*** mkdir: sub dir already exists\n");
			return -EEXIST;
		}
	}

	long free_block_index = find_free_block();	//Get a free block index	TODO error check

	//Update the root dir to contain the new sub dir
	strncpy(root->directories[root->nDirectories].dname, dir, MAX_FILENAME + 1);	//TODO error? remove +1
	root->directories[root->nDirectories].nStartBlock = free_block_index;
	root->nDirectories++;

	// Check if either the bitmap or write to disk functions return an error code
	if(set_bit(free_block_index) < 0 || write_disk_block(0, root) < 0){
		fprintf(stderr, "*** mkdir: error creating the new sub dir!\n");
		return -1;
	}

	if(root != NULL) free(root);

	return res;
}

/*
 * Does the actual creation of a file. Mode and dev can be ignored.
 * ex: echo -n "" > /TESTMOUNT/file.txt (write empty file)
 */
static int cs1550_mknod(const char *path, mode_t mode, dev_t dev)
{
	fprintf(stderr, "*** mknod function call:\n");

	(void) mode;
	(void) dev;
	(void) path;

	root_directory*  root = NULL;
	directory_entry* sub_dir = NULL;;
	index_block*     i_block = NULL;;
	disk_block*			 d_block = NULL;;
	char dir[MAX_FILENAME + 1];		// The directory name
	char fname[MAX_FILENAME + 1];	// The file name
	char fext[MAX_EXTENSION + 1];	// The file extension
	// Init vars to 0
	memset(dir, 0, MAX_FILENAME + 1);
	memset(fname, 0, MAX_FILENAME + 1);
	memset(fext, 0, MAX_EXTENSION + 1);

	int tokens = sscanf(path, "/%[^/]/%[^.].%s", dir, fname, fext);

	if(tokens < 3)	// 3 path tokens are requires to place a file in a sub dir under root
		return -EPERM;

	if(strlen(dir) > MAX_FILENAME || strlen(fname) > MAX_FILENAME || strlen(fext) > MAX_EXTENSION)
		return -ENAMETOOLONG;

	root = get_root_dir();

	if(!root){
		fprintf(stderr, "*** mknod: error reading the root dir\n");
		return -ENOENT;
	}

	// Check for the sub_dir inside root dir:
	int i;
	for(i = 0; i < root->nDirectories; i++)
		if(streq(dir, root->directories[i].dname))
			break;

	int sub_nStartBlock = root->directories[i].nStartBlock;
	sub_dir = get_dir(sub_nStartBlock);

	if(!sub_dir){
		fprintf(stderr, "*** mknod: error reading the sub dir\n");
		return -ENOENT;
	}

	fprintf(stderr, "*** mknod: sub_dir %s returned\n", root->directories[i].dname);

	// Check the sub dir to make sure the file doesnt already exist:
	for(i = 0; i < sub_dir->nFiles; i++)
		if(strneq(fname, sub_dir->files[i].fname, MAX_FILENAME) &&
			 strneq(fext, sub_dir->files[i].fext, MAX_EXTENSION))
			return -EEXIST;

	// Check that the sub dir is not already full:
	if(sub_dir->nFiles == MAX_FILES_IN_DIR)
		return -ENOSPC;

	// Create the new file blocks needed:
	long index_block_num = find_free_block();	// Get a free block index
	set_bit(index_block_num);									// Set index block as used
	long data_block_num  = find_free_block();	// Get a free block index
	set_bit(data_block_num);									// Set data block as used

	fprintf(stderr, "*** mknod: index_block_num = %ld, data_block_num = %ld\n", index_block_num, data_block_num);

	if(!index_block_num || !data_block_num)	//Disk file is full
		return -ENOSPC;

	sub_dir->files[sub_dir->nFiles].nIndexBlock = index_block_num;	// Assign the file's index block
	sub_dir->files[sub_dir->nFiles].fsize = 0;
	strncpy(sub_dir->files[sub_dir->nFiles].fname, fname, MAX_FILENAME + 1);						// Update file name and extension
	strncpy(sub_dir->files[sub_dir->nFiles].fext, fext, MAX_FILENAME + 1);
	sub_dir->nFiles++;

	i_block = calloc(sizeof(index_block), 1);	// Allocate the index block
	i_block->entries[0] = data_block_num;			// Set the index block pointer to the disk block index
	d_block = calloc(sizeof(disk_block), 1);		// Allocate the data block to all 0s
	// memset(d_block, 0, sizeof(disk_block));	// Init the data block to all 0s

	//TODO add error check to unset the bits if these write operations fails

	fprintf(stderr, "*** mknod: writing blocks to disk:\n");

	if(write_disk_block(sub_nStartBlock, sub_dir) < 0)
		fprintf(stderr, "*** mknod: error writing sub_dir!\n");

	if(write_disk_block(index_block_num, i_block) < 0)
		fprintf(stderr, "*** mknod: error writing index_block!\n");

	if(write_disk_block(data_block_num, d_block) < 0)
		fprintf(stderr, "*** mknod: error writing data_block!\n");

	if(root != NULL) 		free(root);
	if(sub_dir != NULL) free(sub_dir);
	if(i_block != NULL) free(i_block);
	if(d_block != NULL) free(d_block);

	return 0;
}



/*
 * Read size bytes from file into buf starting from offset
 * ex: cat /TESTMOUNT/file.txt
 */
static int cs1550_read(const char *path, char *buf, size_t size, off_t offset,
			  struct fuse_file_info *fi)
{
	(void) buf;
	(void) offset;
	(void) fi;
	(void) path;

	fprintf(stderr, "*** read function call: size = %d, offset = %lld\n", size, offset);

	root_directory*  root = NULL;
	directory_entry* sub_dir = NULL;
	disk_block*      file_block = NULL;
	index_block*		 inode = NULL;
	long sub_nStartBlock = 0;
	long file_inode_addr = 0;
	long cur_block_addr = 0;					// The addr of the block where the writing begins
	int cur_block_index = 0;		// The inode index that points to start_block;
	long file_size = 0;

	if(size == 0)	// Nothing to read, return 0
		return 0;

	char dir[MAX_FILENAME + 1];		// The directory name
	char fname[MAX_FILENAME + 1];	// The file name
	char fext[MAX_EXTENSION + 1];	// The file extension
	// Init vars to 0
	memset(dir, 0, MAX_FILENAME + 1);
	memset(fname, 0, MAX_FILENAME + 1);
	memset(fext, 0, MAX_EXTENSION + 1);

	int tokens = sscanf(path, "/%[^/]/%[^.].%s", dir, fname, fext);

	if(tokens < 3)	// 3 path tokens are requires to reference a file
		return -EISDIR;

	// Check file name length
	if(strlen(dir) > MAX_FILENAME || strlen(fname) > MAX_FILENAME || strlen(fext) > MAX_EXTENSION)
		return -ENAMETOOLONG;

	root = get_root_dir();

	// Search for the sub dir
	int i;
	for(i = 0; i < root->nDirectories; i++)
		if(streq(dir, root->directories[i].dname))
			break;

	// load the sub dir
	sub_nStartBlock = root->directories[i].nStartBlock;
	sub_dir = get_dir(sub_nStartBlock);

	if(!sub_dir){
		fprintf(stderr, "*** write: error getting sub dir\n");
		return -ENOENT;
	}

	// Search for the file in the sub dir
	for(i = 0; i < sub_dir->nFiles; i++){
		if(strneq(fname, sub_dir->files[i].fname, MAX_FILENAME) &&
		   strneq(fext, sub_dir->files[i].fext, MAX_EXTENSION)) {
			file_inode_addr = sub_dir->files[i].nIndexBlock;	// Get the block number of the files index block
			file_size = sub_dir->files[i].fsize;
			break;
		}
	}

	if(!file_inode_addr)	// Invalid or nonexisting file index block
		return -ENOENT;

	// Get the files index block
	inode = get_inode(file_inode_addr);

	size_t cur_pos = 0;						// Position in first block where writing begins
	size_t remaining_data = size;	// The data left to write to the file

	// Find the data block that the writing will start in
	for(i = 0; inode->entries[i] != 0 && i < MAX_ENTRIES_IN_INDEX_BLOCK; i++) {

		if(offset <= BLOCK_SIZE) {
			cur_block_addr = inode->entries[i];	// Grab the address of the first data block from inode
			cur_block_index = i;						// Get the index of the block we are starting from
			cur_pos = offset;								// Get the position in the file where the write will begin
			break;
		}
		offset -= BLOCK_SIZE;		// Subtract a block length from the offset to advance forward
	}

	int blocks_to_read = i;

	fprintf(stderr, "*** read: file index_block @ %ld\n", file_inode_addr);
	fprintf(stderr, "*** read: file data block @ %ld\n", cur_block_addr);
	fprintf(stderr, "*** read: start_pos =  %d blocks_to_read = %d\n", cur_pos, blocks_to_read);

	file_block = get_disk_block(cur_block_addr);

	if(size + offset > file_size) {
		size = file_size - offset;
		fprintf(stderr, "*** read: updated read size %ld\n", (long)size);
	}

	for(i = 0; i < size && file_block->data[cur_pos] != 0; i++){

		fprintf(stderr, "*** read: adding %c to buf\n", file_block->data[cur_pos]);
		buf[i] = file_block->data[cur_pos++];

		// Reached end of data block, write to disk and get next block,
		// offset the increment in the [cur_pos++] statement
		if(cur_pos != 0 && cur_pos % (BLOCK_SIZE + 1) == 0) {

			fprintf(stderr, "*** read: reached end of block # %ld\n", cur_block_addr);
			fprintf(stderr, "*** read: i = %d, remaining_data = %d\n", i, remaining_data);

			if(inode->entries[++cur_block_index] != 0){
				fprintf(stderr, "*** write: next block !null, getting block index %d\n", cur_block_index);
				file_block = get_disk_block(inode->entries[cur_block_index]);	// get the next block if it exists
			} else {
				fprintf(stderr, "*** write: next block null, breaking\n");
				break; //TODO ??
			}
		}
		remaining_data--;
	}

	fprintf(stderr, "*** read: buffer contents: |%s|\n", buf);

	if(root != NULL) 		free(root);
	if(sub_dir != NULL) free(sub_dir);
	if(inode != NULL) free(inode);
	if(file_block != NULL) free(file_block);

	return size;
}

/*
 * Write size bytes from buf into file starting from offset
 * ex: echo "Hello World!" > /TESTMOUNT/file.txt
 * ex: cat file.txt > /TESTMOUNT/other_file.txt
 */
static int cs1550_write(const char *path, const char *buf, size_t size,
			  off_t offset, struct fuse_file_info *fi)
{
	(void) buf;
	(void) offset;
	(void) fi;
	(void) path;
	root_directory*  root = NULL;
	directory_entry* sub_dir = NULL;
	disk_block*      file_block = NULL;
	index_block*		 inode = NULL;
	long file_inode_addr = 0;
	long file_old_size = 0;
	long sub_nStartBlock = 0;

	fprintf(stderr, "*** WRITE function call: size = %d, offset = %lld\n", size, offset);

	if(size == 0)	// Nothing to write, return 0
		return -EPERM;

	char dir[MAX_FILENAME + 1];		// The directory name
	char fname[MAX_FILENAME + 1];	// The file name
	char fext[MAX_EXTENSION + 1];	// The file extension
	// Init vars to 0
	memset(dir, 0, MAX_FILENAME + 1);
	memset(fname, 0, MAX_FILENAME + 1);
	memset(fext, 0, MAX_EXTENSION + 1);

	int tokens = sscanf(path, "/%[^/]/%[^.].%s", dir, fname, fext);

	if(tokens < 3)	// 3 path tokens are requires to reference a file
		return -ENOENT;

	// Check file name length
	if(strlen(dir) > MAX_FILENAME || strlen(fname) > MAX_FILENAME || strlen(fext) > MAX_EXTENSION)
		return -ENAMETOOLONG;

	root = get_root_dir();

	// Search for the sub dir
	int i;
	for(i = 0; i < root->nDirectories; i++)
		if(strneq(dir, root->directories[i].dname, MAX_FILENAME))
			break;

	// load the sub dir
	sub_nStartBlock = root->directories[i].nStartBlock;
	sub_dir = get_dir(sub_nStartBlock);

	if(!sub_dir){
		fprintf(stderr, "*** write: error getting sub dir\n");
		return -ENOENT;
	}

	// Search for the file in the sub dir
	for(i = 0; i < sub_dir->nFiles; i++){
		if(strneq(fname, sub_dir->files[i].fname, MAX_FILENAME) &&
			 strneq(fext, sub_dir->files[i].fext, MAX_EXTENSION)) {
			file_inode_addr = sub_dir->files[i].nIndexBlock;	// Get the block number of the files index block
			file_old_size = sub_dir->files[i].fsize;
			sub_dir->files[i].fsize = MAX(file_old_size, (size+offset));	//Update the file size to include the new data
			break;
		}
	}

	if(offset > file_old_size)	// Cant write past the end of the current file data
		return -EFBIG;

	if(!file_inode_addr)	// Invalid or nonexisting file index block
		return -ENOENT;

	// Get the files index block
	inode = get_inode(file_inode_addr);

	//TODO move these declarations to top of function
	long cur_block_addr = 0;					// The addr of the block where the writing begins
	int cur_block_index = 0;		// The inode index that points to start_block;
	size_t remaining_data = size;	// The data left to write to the file
	off_t cur_pos = 0;						// Position in first block where writing begins
	size_t end_pos = file_old_size + remaining_data; // where the write operation will end

	int blocks_needed = ceiling(end_pos, BLOCK_SIZE);	// Round up division since 1 data block is the smallest unit of allocation

	// Find the data block that the writing will start in
	for(i = 0; inode->entries[i] != 0 && i < MAX_ENTRIES_IN_INDEX_BLOCK; i++) {

		if(offset <= BLOCK_SIZE) {
			cur_block_addr = inode->entries[i];	// Grab the address of the first data block from inode
			cur_block_index = i;						// Get the index of the block we are starting from
			cur_pos = offset;								// Get the position in the file where the write will begin
			break;
		}
		offset -= BLOCK_SIZE;		// Subtract a block length from the offset to advance forward
	}

	fprintf(stderr, "*** write: file index_block @ %ld, inode index %d\n", file_inode_addr, cur_block_index);
	fprintf(stderr, "*** write: file data block @ %ld\n", cur_block_addr);
	fprintf(stderr, "*** write: start_pos =  %lld end_pos = %d blocks_needed = %d\n", cur_pos, end_pos, blocks_needed);

		// if(cur_block_addr)
		file_block = get_disk_block(cur_block_addr);

		// Write the data from the buffer into the files data block
		for(i = 0; i < size; i++){
			file_block->data[cur_pos] = buf[i];	// Write to the file's data block from the buffer

			// Check if reached data block boundary is reached
			// offset the increment in the [cur_pos++] statement
			if(cur_pos != 0 && cur_pos % BLOCK_SIZE == 0) {	// Reached end of data block, write to disk and get next block

				fprintf(stderr, "*** write: reached end of block # %ld\n", cur_block_addr);
				fprintf(stderr, "*** write: 	cur_pos = %lld, remaining_data = %d\n", cur_pos, remaining_data);

				if(write_disk_block(cur_block_addr, file_block) < 0)
					fprintf(stderr, "*** write: ERROR writing to disk block %ld \n", cur_block_addr);

				// free(file_block);	// TODO free this block

				if(inode->entries[++cur_block_index] != 0){
					fprintf(stderr, "*** write: next block !null, getting block index %d\n", cur_block_index);
					file_block = get_disk_block(inode->entries[cur_block_index]);	// get the next block if it exists
				} else {

					// Allocate next data block:
					cur_block_addr  = find_free_block();			// Get a free block index	TODO error check
					set_bit(cur_block_addr);									// Set data block as used

					fprintf(stderr, "*** write: next block null, allocating block num %ld at inode[%d]\n", cur_block_addr, cur_block_index);

					file_block = calloc(sizeof(disk_block), 1);	// allocate new block

					inode->entries[cur_block_index] = cur_block_addr;
				}
				cur_pos = 0;	// Reset the current write position to the beginning of new data block
			} else
					cur_pos++;	// Not at block boundary, advance to next byte

			remaining_data--;
		}	// end for

		// Save all changes to disk
		fprintf(stderr, "dir_addr %ld, inode_addr %ld, dataBlock_addr %ld \n", sub_nStartBlock, file_inode_addr, cur_block_addr);

		if(write_disk_block(sub_nStartBlock, sub_dir) < 0)
			fprintf(stderr, "*** mknod: error writing sub_dir!\n");

		if(write_disk_block(file_inode_addr, inode) < 0)
			fprintf(stderr, "*** mknod: error writing index_block!\n");

		if(write_disk_block(cur_block_addr, file_block) < 0)
			fprintf(stderr, "*** write: ERROR writing to disk block!\n");

		if(root != NULL) 		free(root);
		if(sub_dir != NULL) free(sub_dir);
		if(inode != NULL) free(inode);
		if(file_block != NULL) free(file_block);

	return size;
}

/* Initalize the file system, opens disk file and reads the
 * bitmap from disk into memory. Called automatically by fuse_main
 */
static void * cs1550_init(struct fuse_conn_info* fi)
{
	  (void) fi;
    printf("CS 1550 File System init....\n");

		//.disk validation:
		disk = fopen(DISK_FILE_PATH, "rb+");
		if(disk == NULL){
			fprintf(stderr, "***init: Error opening .disk, must init new .disk file\n");
			// Unix command to init a 5 MB .disk file: "dd bs=1K count=5K if=/dev/zero of=.disk"
			exit(1);
		}

		initBitmap();
		return NULL;
}

/* Safely close the file system, closes the disk file
 * and writes the contents of the bitmap to disk to
 * have changes persist until next init.
 * NOTE: CTRL-C interrupt is a safe exit; in the event
 *       of a segfult, run "fusermount -u /TESTMOUNT"
 *       before next init.
 */
static void cs1550_destroy(void* args)
{
		(void) args;

		SEEK_BITMAP
		fwrite(bitmap, BIT_MAP_SIZE, 1, disk);	// Save the bitmap to the disk
		fclose(disk);
		// free(disk);
    printf("\n... CS 1550 File System destroy\n");
}

/* NOTE NOTE NOTE NOTE DO NOT IMPLEMENT THESE FUNCTIONS NOTE NOTE NOTE NOTE */

/*
 * Removes a directory.
 */
static int cs1550_rmdir(const char *path)
{
	(void) path;
    return 0;
}

/*
 * Deletes a file
 */
static int cs1550_unlink(const char *path)
{
    (void) path;

    return 0;
}

/*
 * truncate is called when a new file is created (with a 0 size) or when an
 * existing file is made shorter. We're not handling deleting files or
 * truncating existing ones, so all we need to do here is to initialize
 * the appropriate directory entry.
 *
 */
static int cs1550_truncate(const char *path, off_t size)
{
	(void) path;
	(void) size;

    return 0;
}


/*
 * Called when we open a file
 *
 */
static int cs1550_open(const char *path, struct fuse_file_info *fi)
{
	(void) path;
	(void) fi;
    /*
        //if we can't find the desired file, return an error
        return -ENOENT;
    */

    //It's not really necessary for this project to anything in open

    /* We're not going to worry about permissions for this project, but
	   if we were and we don't have them to the file we should return an error

        return -EACCES;
    */

    return 0; //success!
}

/*
 * Called when close is called on a file descriptor, but because it might
 * have been dup'ed, this isn't a guarantee we won't ever need the file
 * again. For us, return success simply to avoid the unimplemented error
 * in the debug log.
 */
static int cs1550_flush (const char *path , struct fuse_file_info *fi)
{
	(void) path;
	(void) fi;

	return 0; //success!
}



// NOTE END NO MODIFICATION

//register our new functions as the implementations of the syscalls
//NOTE should this be cs1550_oper not hello_oper from the example?
static struct fuse_operations hello_oper = {
    .getattr	= cs1550_getattr,
    .readdir	= cs1550_readdir,
    .mkdir	= cs1550_mkdir,
		.rmdir = cs1550_rmdir,
    .read	= cs1550_read,
    .write	= cs1550_write,
		.mknod	= cs1550_mknod,
		.unlink = cs1550_unlink,
		.truncate = cs1550_truncate,
		.flush = cs1550_flush,
		.open	= cs1550_open,
		.init = cs1550_init,
    .destroy = cs1550_destroy,
};

//Don't change this.
int main(int argc, char *argv[])
{
	return fuse_main(argc, argv, &hello_oper, NULL);
}
