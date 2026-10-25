// ktfs.c - KTFS implementation
//
// Copyright (c) 2024-2025 University of Illinois
// SPDX-License-identifier: NCSA
//

#ifdef KTFS_TRACE
#define TRACE
#endif

#ifdef KTFS_DEBUG
#define DEBUG
#endif


#include "heap.h"
#include "fs.h"
#include "ioimpl.h"
#include "ktfs.h"
#include "error.h"
#include "thread.h"
#include "string.h"
#include "console.h"
#include "cache.h"
#include "assert.h"
#include "thread.h"


// record the files that have been opened, up to 500 files according to PIAZZA
#define KTFS_MAX_OPEN_FILES 500
static struct ktfs_file * opened_files[KTFS_MAX_OPEN_FILES] = {0};

#define KTFS_MAX_INODES 96  // this file system can only have 96 files at most

// INTERNAL TYPE DEFINITIONS
//

// the ktfs file structure
struct ktfs_file {
    // Fill to fulfill spec
    struct io io;                   // the I/O interface of this file
    struct ktfs_dir_entry dentry;   // dentry corresponding to this file, including its inode and name
    struct ktfs_inode inode;        // inode of this file
    unsigned long long filesize;    // the total size of this file
    uint32_t isopen;                // if this file is open, isopen = 1; if it is closed, isopen = 0
};

// define some necessary global variables
static struct {
    struct cache * cache;           // the cache system
    struct ktfs_superblock sb;      // the superblock of this filesystem
    struct lock visit_cache;        // prevent concurrent access to the cache
    struct ktfs_inode root_inode;   // the root directory inode of this file system
} ktfs_global;

// INTERNAL FUNCTION DECLARATIONS
//

int ktfs_mount(struct io * io);

int ktfs_open(const char * name, struct io ** ioptr);
void ktfs_close(struct io* io);
long ktfs_readat(struct io* io, unsigned long long pos, void * buf, long len);
int ktfs_cntl(struct io *io, int cmd, void *arg);

int ktfs_getblksz(struct ktfs_file *fd);
int ktfs_getend(struct ktfs_file *fd, void *arg);

int ktfs_flush(void);

// mp3cp2
long ktfs_writeat(struct io * io, unsigned long long pos, const void * buf, long len);
int ktfs_create(const char * name);
int ktfs_delete(const char * name);

// helper functions
int find_free_block_bit(uint32_t * free_block_number);
int set_data_block(struct ktfs_inode * inode, uint32_t block_in_use, uint32_t block_number, int isroot);

// FUNCTION ALIASES
//

int fsmount(struct io * io)
    __attribute__ ((alias("ktfs_mount")));

int fsopen(const char * name, struct io ** ioptr)
    __attribute__ ((alias("ktfs_open")));

int fsflush(void)
    __attribute__ ((alias("ktfs_flush")));

int fscreate(const char *name)
    __attribute__ ((alias("ktfs_create")));

int fsdelete(const char *name)
    __attribute__ ((alias("ktfs_delete")));
    
// EXPORTED FUNCTION DEFINITIONS
//

/*------------------------------ktfs_mount --------------------------------------
Description:
	Configure filesystem with the provided IO.
Inputs:
    struct io * io: Pointer to the I/O interface 
Outputs:
    Returns 0 on success.
    Returns -EINVAL if the input is invalid or the superblock contains invalid metadata.
    Returns other negative error codes if cache creation or block reading fails
Side Effects:
    Initializes and updates the global ktfs_global structure.
    If a previous filesystem was mounted, it will be overwritten.
----------------------------------------------------------*/
int ktfs_mount(struct io * io)
{
    // check whether the input is valid
    if (io == NULL) {
        return -EINVAL;
    }

    int ret;

    // create a cache and configure this filesystem with the provided IO
    ret = create_cache(io, &ktfs_global.cache);
    // if create_cache fails, return an error code
    if (ret < 0) {
        return ret;
    }

    // initialize the lock
    lock_init(&ktfs_global.visit_cache);
    lock_acquire(&ktfs_global.visit_cache);

    // read the superblock - block 0
    void * blk;
    ret = cache_get_block(ktfs_global.cache, 0, &blk);
    // if fails, return an error code
    if (ret < 0) {
        lock_release(&ktfs_global.visit_cache);
        return ret;
    }
    // copy the read data into the superblock
    memcpy(&ktfs_global.sb, blk, sizeof(struct ktfs_superblock));
    // release this cache block
    cache_release_block(ktfs_global.cache, blk, 0);
    lock_release(&ktfs_global.visit_cache);

    // invaild check：all the block count must > 0
    if (ktfs_global.sb.block_count == 0 || ktfs_global.sb.bitmap_block_count == 0 || ktfs_global.sb.inode_block_count == 0) {
        return -EINVAL;
    }

    // get the root directory inode
    // get the block number of inode_block_0
    uint32_t inode_block_0_num = 1 + ktfs_global.sb.bitmap_block_count;
    // find out which block the root directory inode is in
    uint16_t inodes_per_block = KTFS_BLKSZ / sizeof(struct ktfs_inode);
    uint16_t block_index = ktfs_global.sb.root_directory_inode / inodes_per_block;
    uint16_t block_offset = ktfs_global.sb.root_directory_inode % inodes_per_block;
    unsigned long long pos = (inode_block_0_num + block_index) * KTFS_BLKSZ;
    // read the root directory inode
    lock_acquire(&ktfs_global.visit_cache);
    ret = cache_get_block(ktfs_global.cache, pos, &blk);
    if (ret < 0) {
        lock_release(&ktfs_global.visit_cache);
        return ret;
    }
    memcpy(&ktfs_global.root_inode, blk + block_offset * sizeof(struct ktfs_inode), sizeof(struct ktfs_inode));
    // release this cache block
    cache_release_block(ktfs_global.cache, blk, 0);
    lock_release(&ktfs_global.visit_cache);

    // return 0 on success
    return 0;
}

// helper function for ktfs_open
/*------------------------------------find_dentry ---------------------------------------
Description:
    Searches for a file by name in the root directory of the KTFS filesystem.
    Traverses the direct, indirect, and doubly indirect blocks of the root 
    directory's inode to locate the specified directory entry.
Inputs:
    const char * name: the name of the file to search for.
    struct ktfs_inode root_inode: the root directory inode to begin the search from.
    struct ktfs_dir_entry * dentry : Output pointer. If the file is found, its directory entry is copied into this structure.
Outputs:
    Returns 0 on success (file found).
    Returns -EINVAL if input parameters are invalid.
    Returns -ENOENT if the file is not found.
    Returns -ENODATABLKS if the inodes in use exceeds the maximum number of entries.
    Returns -EIO for any cache read failures.
Side Effects:
    Reads and releases blocks from the cache. Does not allocate memory.
-----------------------------------------------------------------------------------------*/
int find_dentry(const char * name, struct ktfs_inode root_inode, struct ktfs_dir_entry * dentry) {
    // check whether the inputs are valid
    if (name == NULL || dentry == NULL) {
        return -EINVAL;
    }

    unsigned long long pos;
    int ret, i , j;
    void * blk;
    void * indirectblk;
    void * dindirectblk;
    uint32_t block_no, indirectblk_no;

    uint32_t dentry_per_block = KTFS_BLKSZ / sizeof(struct ktfs_dir_entry);
    // max number of dentries that can be stored using only direct blocks
    uint32_t directblk_num = KTFS_NUM_DIRECT_DATA_BLOCKS * dentry_per_block;
    // max number of dentries that can be stored using only direct and indirect blocks
    uint32_t indirectblk_num = directblk_num + KTFS_BLKSZ/sizeof(uint32_t) * dentry_per_block;
    // max number of dentries that can be stored using all kinds of blocks
    uint32_t dindirectblk_num = indirectblk_num + KTFS_NUM_DINDIRECT_BLOCKS * KTFS_BLKSZ/sizeof(uint32_t) * KTFS_BLKSZ/sizeof(uint32_t) * dentry_per_block;

    // the number of inodes in use
    uint32_t entries_num = root_inode.size / sizeof(struct ktfs_dir_entry);
    // if the inodes in use is too many
    if (entries_num > dindirectblk_num) {
        return -ENODATABLKS;
    }

    // get the block number of data_block_0
    uint32_t data_block_0_num = 1 + ktfs_global.sb.bitmap_block_count + ktfs_global.sb.inode_block_count;
    uint32_t dentry_no = 1;   // record the number of the dentry now being read
    struct ktfs_dir_entry cur_dentry;

    // iterate through the 3 direct blocks in turn
    for (i = 0; i < KTFS_NUM_DIRECT_DATA_BLOCKS && dentry_no <= entries_num; i++) {
        // read the direct block
        pos = (data_block_0_num + root_inode.block[i]) * KTFS_BLKSZ;
        ret = cache_get_block(ktfs_global.cache, pos, &blk);
        if (ret < 0) {
            return ret;
        }

        // iterate through each dentry in this direct block, comparing name
        for (j = 0; j < dentry_per_block && dentry_no <= entries_num; j++) {
            // get the current dentry
            memcpy(&cur_dentry, blk + j * sizeof(struct ktfs_dir_entry), sizeof(struct ktfs_dir_entry));
            // if the correct file is found
            if (strncmp(name, cur_dentry.name, sizeof(cur_dentry.name)) == 0) {
                *dentry = cur_dentry;

                cache_release_block(ktfs_global.cache, blk, 0);
                // return 0 on success
                return 0;
            }
            // go to the next dentry
            dentry_no++;
        }
        // release this cache block
        cache_release_block(ktfs_global.cache, blk, 0);
    }


    // iterate through the indirect block (there is only one indirect block)
    // read the indirect block
    pos = (data_block_0_num + root_inode.indirect) * KTFS_BLKSZ;
    ret = cache_get_block(ktfs_global.cache, pos, &indirectblk);
    if (ret < 0) {
        return ret;
    }
    // read every data block in the indirect block
    for (i = 0; i < KTFS_BLKSZ/sizeof(uint32_t) && dentry_no <= entries_num; i++) {
        memcpy(&block_no, indirectblk + i * sizeof(uint32_t), sizeof(uint32_t));

        // read the data block
        pos = (data_block_0_num + block_no) * KTFS_BLKSZ;
        ret = cache_get_block(ktfs_global.cache, pos, &blk);
        if (ret < 0) {
            cache_release_block(ktfs_global.cache, indirectblk, 0);
            return ret;
        }

        // iterate through each dentry in this data block, comparing name
        for (j = 0; j < dentry_per_block && dentry_no <= entries_num; j++) {
            // get the current dentry
            memcpy(&cur_dentry, blk + j * sizeof(struct ktfs_dir_entry), sizeof(struct ktfs_dir_entry));
            // if the correct file is found
            if (strncmp(name, cur_dentry.name, sizeof(cur_dentry.name)) == 0) {
                *dentry = cur_dentry;

                cache_release_block(ktfs_global.cache, blk, 0);
                cache_release_block(ktfs_global.cache, indirectblk, 0);
                // return 0 on success
                return 0;
            }

            // go to the next dentry
            dentry_no++;
        }

        // release this cache block
        cache_release_block(ktfs_global.cache, blk, 0);
    }
    // release this cache block
    cache_release_block(ktfs_global.cache, indirectblk, 0);
    // iterate through the 2 double-indirect blocks in turn
    for (int a = 0; a < KTFS_NUM_DINDIRECT_BLOCKS && dentry_no <= entries_num; a++) {
        // read the double-indirect block
        pos = (data_block_0_num + root_inode.dindirect[a]) * KTFS_BLKSZ;
        ret = cache_get_block(ktfs_global.cache, pos, &dindirectblk);
        if (ret < 0) {
            return ret;
        }
        // read every indirect block in the double-indirect block
        for (int b = 0; b < KTFS_BLKSZ/sizeof(uint32_t) && dentry_no <= entries_num; b++) {
            // read the indirect block
            memcpy(&indirectblk_no, dindirectblk + b * sizeof(uint32_t), sizeof(uint32_t));
            pos = (data_block_0_num + indirectblk_no) * KTFS_BLKSZ;
            ret = cache_get_block(ktfs_global.cache, pos, &indirectblk);
            if (ret < 0) {
                cache_release_block(ktfs_global.cache, dindirectblk, 0);
                return ret;
            }
            // read every data block in the indirect block
            for (i = 0; i < KTFS_BLKSZ/sizeof(uint32_t) && dentry_no <= entries_num; i++) {
                memcpy(&block_no, indirectblk + i * sizeof(uint32_t), sizeof(uint32_t));
                // read the data block
                pos = (data_block_0_num + block_no) * KTFS_BLKSZ;
                ret = cache_get_block(ktfs_global.cache, pos, &blk);
                if (ret < 0) {
                    cache_release_block(ktfs_global.cache, indirectblk, 0);
                    cache_release_block(ktfs_global.cache, dindirectblk, 0);
                    return ret;
                }
                // iterate through each dentry in this data block, comparing name
                for (j = 0; j < dentry_per_block && dentry_no <= entries_num; j++) {
                    // get the current dentry
                    memcpy(&cur_dentry, blk + j * sizeof(struct ktfs_dir_entry), sizeof(struct ktfs_dir_entry));
                    // if the correct file is found
                    if (strncmp(name, cur_dentry.name, sizeof(cur_dentry.name)) == 0) {
                        *dentry = cur_dentry;
                        cache_release_block(ktfs_global.cache, blk, 0);
                        cache_release_block(ktfs_global.cache, indirectblk, 0);
                        cache_release_block(ktfs_global.cache, dindirectblk, 0);
                        // return 0 on success
                        return 0;
                    }
                    // go to the next dentry
                    dentry_no++;
                }
                // release this cache block
                cache_release_block(ktfs_global.cache, blk, 0);
            }
            // release this cache block
            cache_release_block(ktfs_global.cache, indirectblk, 0);
        }
        // release this cache block
        cache_release_block(ktfs_global.cache, dindirectblk, 0);
    }
    // file not found
    return -ENOENT;
}

/*-----------------------------------ktfs_open --------------------------------------
Description:
 	Open a file by name.
Inputs:
    const char * name: the name of the file to open. Must be a null-terminated string.
    struct io ** ioptr: output pointer to store the initialized io structure representing the file.
Outputs:
        Returns 0 on success.
        Returns -EINVAL if input parameters are invalid.
        Returns -ENOENT if the file is not found.
        Returns -EMFILE if there are too many open files.
        Returns -EIO for any cache or read errors.
Side Effects:
    Fills in the file's inode, directory entry, size, and io interface.
----------------------------------------------------------*/
int ktfs_open(const char *name, struct io **ioptr) {
    // check whether the inputs are valid
    if (name == NULL || ioptr == NULL) {
        return -EINVAL;
    }

    int i;
    // if a file marked as in-use is opened again, return an error
    for (i = 0; i < KTFS_MAX_OPEN_FILES; i++) {
        if (opened_files[i] != NULL && strncmp(name, opened_files[i]->dentry.name, sizeof(opened_files[i]->dentry.name)) == 0 && opened_files[i]->isopen == 1) {
            return -EBUSY;
        }
    }

    int ret;
    // get the block number of inode_block_0
    uint32_t inode_block_0_num = 1 + ktfs_global.sb.bitmap_block_count;
    // find out which block the root directory inode is in
    uint16_t inodes_per_block = KTFS_BLKSZ / sizeof(struct ktfs_inode);

    // search the root directory for this specified file
    struct ktfs_dir_entry cur_dentry;
    lock_acquire(&ktfs_global.visit_cache);
    ret = find_dentry(name, ktfs_global.root_inode, &cur_dentry);
    lock_release(&ktfs_global.visit_cache);
    if (ret < 0) {
        return ret;
    }

    // create a new file
    struct ktfs_file * file = kmalloc(sizeof(struct ktfs_file));
    // if the allocation fails, return
    if (file == NULL) {
        return -ENOMEM;
    }
    // initialize newly allocated memory to 0
    memset(file, 0, sizeof(struct ktfs_file));

    struct iointf * file_intf = (struct iointf *) kmalloc(sizeof(struct iointf));
    // if the allocation fails, free previously allocated memory and return
    if (file_intf == NULL) {
        kfree(file);
        return -ENOMEM;
    }
    // initialize newly allocated memory to 0
    memset(file_intf, 0, sizeof(struct iointf));

    // find an empty place in the opened_files array to insert the new file
    int inserted = 0;
    for (i = 0; i < KTFS_MAX_OPEN_FILES; i++) {
        if (opened_files[i] == NULL) {
            opened_files[i] = file;
            file->isopen = 1;
            inserted = 1;
            break;
        }
    }
    // if there's no more empty place
    if (inserted == 0) {
        kfree(file_intf);
        kfree(file);
        return -EMFILE;
    }

    // initialize this new file
    file->dentry = cur_dentry;
    file->io.refcnt = 1;
    file_intf->close = ktfs_close;
    file_intf->readat = ktfs_readat;
    file_intf->cntl = ktfs_cntl;
    file_intf->writeat = ktfs_writeat;
    file->io.intf = file_intf;

    // find the inode of this file
    uint16_t block_index = file->dentry.inode / inodes_per_block;
    //assert(block_index < ktfs_global.sb.inode_block_count);
    uint16_t block_offset = file->dentry.inode % inodes_per_block;
    unsigned long long pos = (inode_block_0_num + block_index) * KTFS_BLKSZ;
    // read the block where the inode is located
    lock_acquire(&ktfs_global.visit_cache);
    void * blk;
    ret = cache_get_block(ktfs_global.cache, pos, &blk);
    if (ret < 0) {
        lock_release(&ktfs_global.visit_cache);
        return ret;
    }
    struct ktfs_inode file_inode;
    memcpy(&file_inode, blk + block_offset * sizeof(struct ktfs_inode), sizeof(struct ktfs_inode));
    // release this cache block
    cache_release_block(ktfs_global.cache, blk, 0);
    lock_release(&ktfs_global.visit_cache);
    file->inode = file_inode;

    // initialize other parameters
    file->filesize = file_inode.size;

    // return the io function pointer to interface with the requested file
    *ioptr = create_seekable_io(&file->io);

    // return 0 on success
    return 0;
}

/*--------------------------------------ktfs_close---------------------------------------
Description:
    Mark the ktfs_file associated with ioptr as not in use.
Inputs:
    struct io * io: the io interface pointer of the file to be closed.
Outputs:
        None
Side Effects:
    Frees the ktfs_file and iointf structures.
    Removes the file from the opened_files array.
----------------------------------------------------------------------------------------*/
void ktfs_close(struct io* io)
{
    // check whether the input is valid
    if (io == NULL) {
        return;
    }
    // search for the file in the opened_files array
    for (int i = 0; i < KTFS_MAX_OPEN_FILES; i++) {
        // if the correct file is found
        if (opened_files[i] != NULL && &opened_files[i]->io == io) {
            // mark file as closed
            opened_files[i]->isopen = 0;
            // free the iointf structure
            if (opened_files[i]->io.intf != NULL) {
                struct iointf * intf = (struct iointf *) opened_files[i]->io.intf;
                kfree(intf);
                opened_files[i]->io.intf = NULL;
            }
            // free the ktfs_file structure
            kfree(opened_files[i]);
            // remove this file from the opened_files array
            opened_files[i] = NULL;
            return;
        }
    }
}

// helper function for ktfs_readat
/*--------------------------------- get_data_block ---------------------------------------
Description:
    Resolves the physical data block number corresponding to the k-th logical
    data block in a given inode.
Inputs:
    struct ktfs_inode * inode: pointer to the inode containing the block mapping information.
    int k:the logical block index to resolve.
    uint32_t * blockno: Output pointer where the resolved physical block number will be stored.
Outputs:
    0 on success.
    -ENODATABLKS if the requested block index is out of bounds.
    Negative error code from cache_get_block() if a block access fails.
Side Effects:
    Reads indirect or doubly-indirect blocks from cache if needed.
------------------------------------------------------------------------------------------*/
static int get_data_block(struct ktfs_inode * inode, int k, uint32_t * blockno) {
    int ret;
    unsigned long long pos;

    // case 1: k is in the direct blocks
    if (k < KTFS_NUM_DIRECT_DATA_BLOCKS) {
        *blockno = inode->block[k];
        return 0;
    }
    // decrease k
    k -= KTFS_NUM_DIRECT_DATA_BLOCKS;

    // get the block number of data_block_0
    uint32_t data_block_0_num = 1 + ktfs_global.sb.bitmap_block_count + ktfs_global.sb.inode_block_count;

    // case 2: k is in the indirect block
    if (k < KTFS_BLKSZ / sizeof(uint32_t)) {
        // read the indirect block
        pos = (data_block_0_num + inode->indirect) * KTFS_BLKSZ;
        void * blk;
        lock_acquire(&ktfs_global.visit_cache);
        ret = cache_get_block(ktfs_global.cache, pos, &blk);
        if (ret < 0) {
            lock_release(&ktfs_global.visit_cache);
            return ret;
        }

        // return the k-th block number
        *blockno = ((uint32_t *)blk)[k];

        // release this cache block
        cache_release_block(ktfs_global.cache, blk, 0);
        lock_release(&ktfs_global.visit_cache);

        return 0;
    }
    // decrease k
    k -= KTFS_BLKSZ / sizeof(uint32_t);

    // case 3: k is in the doubly-indirect block
    // the k-th block index in the doubly-indirect block
    int dindirect_idx = k / ((KTFS_BLKSZ / sizeof(uint32_t)) * (KTFS_BLKSZ / sizeof(uint32_t)));
    // the k-th block offset in the doubly-indirect block
    int dindirect_offset = k % ((KTFS_BLKSZ / sizeof(uint32_t)) * (KTFS_BLKSZ / sizeof(uint32_t)));
    // the k-th block index in the indirect block
    int indirect_idx = dindirect_offset / (KTFS_BLKSZ / sizeof(uint32_t));
    // the k-th block offset in the indirect block
    int indirect_offset = dindirect_offset % (KTFS_BLKSZ / sizeof(uint32_t));

    // if the index is out of bounds
    if (dindirect_idx >= KTFS_NUM_DINDIRECT_BLOCKS) {
        return -ENODATABLKS;
    }

    // read the doubly-indirect block
    pos = (data_block_0_num + inode->dindirect[dindirect_idx]) * KTFS_BLKSZ;
    void * dblk;
    lock_acquire(&ktfs_global.visit_cache);
    ret = cache_get_block(ktfs_global.cache, pos, &dblk);
    if (ret < 0) {
        lock_release(&ktfs_global.visit_cache);
        return ret;
    }

    // get the specified indirect block number
    uint32_t indirect_block_num = ((uint32_t *)dblk)[indirect_idx];

    // release this cache block
    cache_release_block(ktfs_global.cache, dblk, 0);
    lock_release(&ktfs_global.visit_cache);

    
    // read the indirect block
    pos = (data_block_0_num + indirect_block_num) * KTFS_BLKSZ;
    void * iblk;
    lock_acquire(&ktfs_global.visit_cache);
    ret = cache_get_block(ktfs_global.cache, pos, &iblk);
    if (ret < 0) {
        lock_release(&ktfs_global.visit_cache);
        return ret;
    }

    // return the k-th block number
    *blockno = ((uint32_t *)iblk)[indirect_offset];

    // release this cache block
    cache_release_block(ktfs_global.cache, iblk, 0);
    lock_release(&ktfs_global.visit_cache);

    return 0;
}

/*----------------------------------- ktfs_readat ----------------------------------------
Description:
    Read len bytes starting at pos from the file associated with io.
Inputs:
    struct io * io: pointer to the I/O interface of the opened KTFS file.
    unsigned long long pos: offset in the file to start reading from.
    void * buf: destination buffer to store the read data.
    long len: Number of bytes to read.
Outputs:
    Returns the number of bytes actually read on success.
    Returns -EINVAL if inputs are invalid or out of bounds.
    Returns other negative error codes if cache or block access fails.
Side Effects:
    Modifies the provided buffer with file data.
----------------------------------------------------------------------------------------*/
long ktfs_readat(struct io* io, unsigned long long pos, void * buf, long len)
{
    // check whether the inputs are valid
    if (io == NULL || buf == NULL || len < 0) {
        return -EINVAL;
    }

    int ret;
    // find the corresponding file
    struct ktfs_file * file = (void*)io - offsetof(struct ktfs_file, io);

    // out-of-bounds reads are not allowed
    if (pos >= file->filesize) {
        return 0;
    }
    // adjust len to avoid reading past end-of-file
    if (pos + len > file->filesize) {
        len = file->filesize - pos;
    }
    uint8_t * dest = (uint8_t *) buf;   // the destination buffer
    long total_read = 0;                // the total bytes we have read

    // read in a loop until all requested bytes are reads
    while (total_read < len) {
        // currently reading data block
        int blk_idx = (pos + total_read) / KTFS_BLKSZ;
        // offset within the current block where reading should begin
        int blk_offset = (pos + total_read) % KTFS_BLKSZ;

        // number of bytes to read from this block
        int read_len = KTFS_BLKSZ - blk_offset;
        if (read_len > len - total_read) {
            read_len = len - total_read;
        }

        // get the actual data block number from the inode
        uint32_t blkno;
        ret = get_data_block(&file->inode, blk_idx, &blkno);
        // return an error code if fails
        if (ret != 0) {
            return ret;
        }

        // get the block number of data_block_0
        uint32_t data_block_0_num = 1 + ktfs_global.sb.bitmap_block_count + ktfs_global.sb.inode_block_count;
        // read the specified data block
        unsigned long long blk_pos = (data_block_0_num + blkno) * KTFS_BLKSZ;
        void *blk;
        lock_acquire(&ktfs_global.visit_cache);
        ret = cache_get_block(ktfs_global.cache, blk_pos, &blk);
        if (ret < 0) {
            lock_release(&ktfs_global.visit_cache);
            return ret;
        }
        // copy the data to the destination buffer
        memcpy(dest + total_read, (uint8_t *)blk + blk_offset, read_len);

        // release this cache block
        cache_release_block(ktfs_global.cache, blk, 0);
        lock_release(&ktfs_global.visit_cache);

        // update the total bytes read
        total_read += read_len;
    }

    return total_read;
}

// helper function for ktfs_cntl - SETEND
/*----------------------------------- extend_file ----------------------------------------
Description:
    Extends the file to the specified new size by allocating new data blocks if needed.
    Only supports increasing the file size; shrinking is not supported.
Inputs:
    struct ktfs_file * file: File to extend.
    void * arg: Pointer to unsigned long long specifying the new file size.
Outputs:
    Returns 0 on success.
    Returns -ENOTSUP if shrinking is attempted.
    Returns other error codes on allocation or disk error.
Side Effects:
    May modify the file's inode, data blocks, and disk bitmap.
----------------------------------------------------------------------------------------*/
int extend_file(struct ktfs_file * file, void * arg) {
    // pointer to the file size to be set
    unsigned long long new_size = *(unsigned long long *) arg;

    int ret;

    // this function can only extend the file
    if (new_size < file->filesize) {
        return -ENOTSUP;
    }
    if (new_size == file->filesize) {
        return 0;
    }
        
    uint32_t old_blocks = (file->filesize + KTFS_BLKSZ - 1) / KTFS_BLKSZ;   // number of data blocks already available
    uint32_t new_blocks = (new_size + KTFS_BLKSZ - 1) / KTFS_BLKSZ;         // number of data blocks needed


    // check whether we need to add additional data blocks
    if (old_blocks == new_blocks) {
        goto update_size;
    }
    
    uint32_t new_blkno;
    volatile uint32_t i;
    // add additional data blocks to this file
    for (i = old_blocks; i < new_blocks; i++) {
        // first find a free block and update the bitmap
        ret = find_free_block_bit(&new_blkno);
        if (ret < 0) {
            return ret;
        }
        // write this new block number into the inode
        ret = set_data_block(&file->inode, i, new_blkno, 0);
        if (ret < 0) {
            return ret;
        }
    }

update_size:
    // update the file length
    file->filesize = new_size;
    file->inode.size = new_size;
    // write the new length back to the disk
    void * blk;
    // get the block number of inode_block_0
    uint32_t inode_block_0_num = 1 + ktfs_global.sb.bitmap_block_count;
    // find out which block the file inode is in
    uint16_t inodes_per_block = KTFS_BLKSZ / sizeof(struct ktfs_inode);
    uint16_t inode_block_index = file->dentry.inode / inodes_per_block;
    uint16_t inode_block_offset = file->dentry.inode % inodes_per_block;
    unsigned long long pos = (inode_block_0_num + inode_block_index) * KTFS_BLKSZ;
    // read the inode
    lock_acquire(&ktfs_global.visit_cache);
    ret = cache_get_block(ktfs_global.cache, pos, &blk);
    if (ret < 0) {
        lock_release(&ktfs_global.visit_cache);
        return ret;
    }

    // update the new size in the disk
    struct ktfs_inode * actual_inode = (struct ktfs_inode *) (blk + inode_block_offset * sizeof(struct ktfs_inode));
    actual_inode->size = new_size;

    // release this cache block
    cache_release_block(ktfs_global.cache, blk, 1);
    lock_release(&ktfs_global.visit_cache);

    // flush the cache
    //ktfs_flush();

    // return 0 on success
    return 0;
}

/*-----------------------------------ktfs_cntl------------------------------------------
Description:
    Do an special / ioctl function on the filesystem. The action to take is determined by the value of cmd.
Inputs:
    struct io * io: the IO interface of the KTFS file.
    int cmd: The control command to execute.
    void * arg: pointer to return or pass information, depending on cmd.
Outputs:
    Returns 0 on success.
    Returns -ENOTSUP for unsupported commands.
    Returns -EINVAL if arguments are invalid.
Side Effects:
    None
----------------------------------------------------------------------------------------*/
int ktfs_cntl(struct io *io, int cmd, void *arg)
{
    // check whether the inputs are valid
    if (io == NULL) {
        return -EINVAL;
    }

    // find the corresponding file
    struct ktfs_file * file = (void*)io - offsetof(struct ktfs_file, io);
    switch (cmd) {
        // get the size of the file
        case IOCTL_GETEND:
            if (arg == NULL) {
                return -EINVAL;
            }
            *(unsigned long long *) arg = file->filesize;
            return 0;
        // set the file size, that is, extend the file
        case IOCTL_SETEND:
            if (arg == NULL) {
                return -EINVAL;
            }
            return extend_file(file, arg);
        // get the block size, return the block size directly
        case IOCTL_GETBLKSZ:
            return 1;   // we can read any byte of data in ktfs
        // unsupported command
        default:
            return -ENOTSUP;
    }
}

/*-------------------------------------ktfs_flush--------------------------------------
Description:
    Flush the cache to the backing device.
Inputs:
    None
Outputs:
    Returns 0 if all dirty cache blocks are successfully flushed to the backing device.
    Returns negative error codes if there's an error.
Side Effects:
    None
--------------------------------------------------------------------------------------*/
int ktfs_flush(void)
{
    // check whether the cache system is valid
    if (ktfs_global.cache == NULL) {
        return -EINVAL;
    }
    lock_acquire(&ktfs_global.visit_cache);
    // flush the cache
    int ret = cache_flush(ktfs_global.cache);
    lock_release(&ktfs_global.visit_cache);

    return ret;
}




/* mp3cp2 */
/*----------------------------------- ktfs_writeat ----------------------------------------
Description:
    Write len bytes starting at pos to the file associated with io.
    This function truncates writes at the end of the file, as denoted by the file size.
Inputs:
    struct io * io: pointer to the I/O interface of the opened KTFS file.
    unsigned long long pos: offset in the file to start writing at.
    const void * buf: source buffer containing the data to be written.
    long len: number of bytes to write from the buffer into the file.
Outputs:
    Returns the number of bytes actually written on success.
    Returns -EINVAL if inputs are invalid or out of bounds.
    Returns other negative error codes if cache or block access fails.
Side Effects:
    Modifies the contents of the file on disk.
----------------------------------------------------------------------------------------*/
long ktfs_writeat(struct io * io, unsigned long long pos, const void * buf, long len)
{
    // check whether the inputs are valid
    if (io == NULL || buf == NULL || len < 0) {
        return -EINVAL;
    }

    int ret;

    // find the corresponding file
    struct ktfs_file * file = (void*)io - offsetof(struct ktfs_file, io);

    // out-of-bounds writes are not allowed
    if (pos >= file->filesize) {
        return 0;
    }
    // adjust len to avoid writing past end-of-file
    if (pos + len > file->filesize) {
        len = file->filesize - pos;
    }

    const uint8_t * src = (const uint8_t *) buf;    // the source buffer
    long total_written = 0;                         // the total bytes we have written

    // write in a loop until all requested bytes are written
    while (total_written < len) {
        // currently writing data block
        int blk_idx = (pos + total_written) / KTFS_BLKSZ;
        // offset within the current block where writing should begin
        int blk_offset = (pos + total_written) % KTFS_BLKSZ;

        // number of bytes to write to this block
        int write_len = KTFS_BLKSZ - blk_offset;
        if (write_len > len - total_written) {
            write_len = len - total_written;
        }

        // get the actual data block number from the inode
        uint32_t blkno;
        ret = get_data_block(&file->inode, blk_idx, &blkno);
        // return an error code if fails
        if (ret != 0) {
            return ret;
        }

        // get the block number of data_block_0
        uint32_t data_block_0_num = 1 + ktfs_global.sb.bitmap_block_count + ktfs_global.sb.inode_block_count;
        // get the specified data block
        unsigned long long blk_pos = (data_block_0_num + blkno) * KTFS_BLKSZ;
        void *blk;
        lock_acquire(&ktfs_global.visit_cache);
        ret = cache_get_block(ktfs_global.cache, blk_pos, &blk);
        if (ret < 0) {
            lock_release(&ktfs_global.visit_cache);
            return ret;
        }
        // copy the data from the source buffer to the file
        memcpy((uint8_t *)blk + blk_offset, src + total_written, write_len);

        // release this cache block and set the dirty bit to write back
        cache_release_block(ktfs_global.cache, blk, 1);
        lock_release(&ktfs_global.visit_cache);

        // update the total bytes written
        total_written += write_len;
    }

    // write back to the backing device
    //ktfs_flush();

    return total_written;
}

// helper function
/*------------------------------ find_free_block_bit ------------------------------------
Description:
    Scan through the KTFS bitmap blocks to find the first unused data block.
    Marks the located bit as used in the bitmap and writes the updated bitmap
    back to disk.
Inputs:
    uint32_t * free_block_number: pointer to store the index of the first unused block
                                  found in the filesystem.
Outputs:
    Returns 0 on success, and the block number is stored in *free_block_number.
    Returns -ENODATABLKS if no free block is available.
    Returns other negative error codes if cache access fails.
Side Effects:
    Modifies the bitmap block in memory and on disk by marking a block as used.
----------------------------------------------------------------------------------------*/
int find_free_block_bit(uint32_t * free_block_number) {
    void * blk;
    int ret;

    uint32_t bits_per_block = KTFS_BLKSZ * 8;   // 1 byte = 8 bits
    uint32_t total_bits, byte_idx, bit_idx;

    // global block number of the starting data block
    uint32_t data_block_start = 1 + ktfs_global.sb.bitmap_block_count + ktfs_global.sb.inode_block_count;
    uint32_t data_block_count = ktfs_global.sb.block_count - data_block_start;

    // determine how many bitmap blocks will be used in total
    uint32_t valid_bitmap_blk = (ktfs_global.sb.block_count + bits_per_block - 1) / bits_per_block;
    //assert(valid_bitmap_blk <= ktfs_global.sb.bitmap_block_count);

    // iterate through every bitmap block
    for (int i = 0; i < valid_bitmap_blk; i++) {
        // read the i-th bitmap block
        lock_acquire(&ktfs_global.visit_cache);
        ret = cache_get_block(ktfs_global.cache, (1 + i) * KTFS_BLKSZ, &blk);
        // if fails
        if (ret < 0) {
            lock_release(&ktfs_global.visit_cache);
            return ret;
        }

        // determine how many bits in the last valid bitmap block is valid
        if (i == valid_bitmap_blk - 1) {
            total_bits = ktfs_global.sb.block_count % bits_per_block;
            // if the last block is just filled up
            if (total_bits == 0) {
                total_bits = bits_per_block;
            }
        }
        else {
            total_bits = bits_per_block;
        }

        uint8_t * bitmap = (uint8_t *) blk;
        // iterate through every valid bit in this bitmap block
        for (uint32_t j = 0; j < total_bits; j++) {
            byte_idx = j / 8;
            bit_idx  = j % 8;

            uint32_t n = i * bits_per_block + j;
            // skip non-data blocks
            if (n < data_block_start) {
                continue;
            }
            // skip out of data block range
            uint32_t data_block_number = n - data_block_start;
            if (data_block_number >= data_block_count) {
                continue;
            }

            // if the free block is found
            if ((bitmap[byte_idx] & (1 << bit_idx)) == 0) {
                // mark it as used
                bitmap[byte_idx] |= (1 << bit_idx);
                // return the corresponding data block number
                *free_block_number = data_block_number;

                // write back to the disk
                cache_release_block(ktfs_global.cache, blk, 1);
                lock_release(&ktfs_global.visit_cache);

                // return 0 on success
                return 0;
            }
        }

        // release this bitmap block
        cache_release_block(ktfs_global.cache, blk, 0);
        lock_release(&ktfs_global.visit_cache);
    }

    // no data blocks available
    return -ENODATABLKS;
}

// helper function
/*----------------------------- set_data_block -----------------------------
Description:
    Set the data block pointer in a KTFS inode, supporting direct, indirect,
    and double indirect addressing. Allocates indirect/double-indirect blocks
    if needed and updates the on-disk inode.
Inputs:
    struct ktfs_inode *inode: inode to modify
    uint32_t block_in_use: logical block index to set
    uint32_t block_number: physical data block number to assign
    int isroot: 1 if this is the root inode, else 0
Outputs:
    Returns 0 on success.
    Returns negative error codes on failure.
Side Effects:
    May allocate new pointer blocks; modifies and writes back inodes and
    pointer blocks on disk.
-------------------------------------------------------------------------*/
int set_data_block(struct ktfs_inode * inode, uint32_t block_in_use, uint32_t block_number, int isroot) {
    void *blk;
    int ret;
    unsigned long long pos;
    struct ktfs_inode * actual_inode;

    // get the root directory inode
    // get the block number of inode_block_0
    uint32_t inode_block_0_num = 1 + ktfs_global.sb.bitmap_block_count;
    // find out which block the root directory inode is in
    uint16_t inodes_per_block = KTFS_BLKSZ / sizeof(struct ktfs_inode);
    uint16_t inode_block_index = ktfs_global.sb.root_directory_inode / inodes_per_block;
    uint16_t inode_block_offset = ktfs_global.sb.root_directory_inode % inodes_per_block;

    // check whether this inode is the root; if not, change the index and offset
    if (isroot == 0) {
        // find the corresponding file
        struct ktfs_file * file = (struct ktfs_file *)((char *)inode - offsetof(struct ktfs_file, inode));
        inode_block_index = file->dentry.inode / inodes_per_block;
        inode_block_offset = file->dentry.inode % inodes_per_block;
    }

    // get the position of this inode in the disk
    unsigned long long inode_pos = (inode_block_0_num + inode_block_index) * KTFS_BLKSZ;

    // get the block number of data_block_0
    uint32_t data_block_0_num = 1 + ktfs_global.sb.bitmap_block_count + ktfs_global.sb.inode_block_count;


    // if the data block pointer we need to set is in the direct blocks 
    if (block_in_use < KTFS_NUM_DIRECT_DATA_BLOCKS) {
        inode->block[block_in_use] = block_number;

        // write back to the disk
        // read the inode
        lock_acquire(&ktfs_global.visit_cache);
        ret = cache_get_block(ktfs_global.cache, inode_pos, &blk);
        if (ret < 0) {
            lock_release(&ktfs_global.visit_cache);
            return ret;
        }

        actual_inode = (struct ktfs_inode *) (blk + inode_block_offset * sizeof(struct ktfs_inode));
        actual_inode->block[block_in_use] = block_number;

        // release this cache block
        cache_release_block(ktfs_global.cache, blk, 1);
        lock_release(&ktfs_global.visit_cache);
        return 0;
    }


    // number of data block pointers per block
    uint32_t ptrs_per_block = KTFS_BLKSZ / sizeof(uint32_t);
    // if the data block pointer we need to set is in the indirect blocks
    block_in_use -= 3;
    if (block_in_use < ptrs_per_block) {
        // if the indirect block doesn't exist, allocate one
        if (block_in_use == 0) {
            uint32_t tmp_indirect;
            // first find a free block
            ret = find_free_block_bit(&tmp_indirect);
            if (ret < 0) {
                return ret;
            }
            // set the indirect block number
            inode->indirect = tmp_indirect;

            // write the indirect block number back to the disk
            // read the inode
            lock_acquire(&ktfs_global.visit_cache);
            ret = cache_get_block(ktfs_global.cache, inode_pos, &blk);
            if (ret < 0) {
                lock_release(&ktfs_global.visit_cache);
                return ret;
            }

            actual_inode = (struct ktfs_inode *) (blk + inode_block_offset * sizeof(struct ktfs_inode));
            actual_inode->indirect = inode->indirect;

            // release this cache block
            cache_release_block(ktfs_global.cache, blk, 1);
            lock_release(&ktfs_global.visit_cache);

            // initialize the newly allocated indirect block
            pos = (data_block_0_num + inode->indirect) * KTFS_BLKSZ;
            lock_acquire(&ktfs_global.visit_cache);
            ret = cache_get_block(ktfs_global.cache, pos, &blk);
            if (ret < 0) {
                lock_release(&ktfs_global.visit_cache);
                return ret;
            }
            memset(blk, 0, KTFS_BLKSZ);
            cache_release_block(ktfs_global.cache, blk, 1);
            lock_release(&ktfs_global.visit_cache);
        }

        // write the block_number into the indirect block
        unsigned long long indirect_pos = (data_block_0_num + inode->indirect) * KTFS_BLKSZ;
        lock_acquire(&ktfs_global.visit_cache);
        ret = cache_get_block(ktfs_global.cache, indirect_pos, &blk);
        if (ret < 0) {
            lock_release(&ktfs_global.visit_cache);
            return ret;
        }
        uint32_t * table = (uint32_t *) blk;
        table[block_in_use] = block_number;
        cache_release_block(ktfs_global.cache, blk, 1);
        lock_release(&ktfs_global.visit_cache);
        return 0;
    }


    // if the data block pointer we need to set is in the doubly-indirect blocks
    block_in_use -= ptrs_per_block;
    // the maximum logical block number supported
    uint32_t total_dindirect_ptrs = ptrs_per_block * ptrs_per_block * KTFS_NUM_DINDIRECT_BLOCKS;
    // check whether it exceeds the maximum logical block number supported
    if (block_in_use >= total_dindirect_ptrs) {
        return -EINVAL;
    }

    // which doubly-indirect block should the block_number go into
    uint32_t which_dindirect = block_in_use / (ptrs_per_block * ptrs_per_block);
    uint32_t index_within_dindirect = block_in_use % (ptrs_per_block * ptrs_per_block);
    // offset in the doubly-indirect block
    uint32_t level1_index = index_within_dindirect / ptrs_per_block;
    // offset in the indirect block
    uint32_t level2_index = index_within_dindirect % ptrs_per_block;

    // if the doubly-indirect block doesn't exist, allocate one
    if (index_within_dindirect == 0) {
        uint32_t tmp_dindirect;
        // first find a free block
        ret = find_free_block_bit(&tmp_dindirect);
        if (ret < 0) {
            return ret;
        }
        // set the dindirect block number
        inode->dindirect[which_dindirect] = tmp_dindirect;

        // write the dindirect block number back to the disk
        // read the inode
        lock_acquire(&ktfs_global.visit_cache);
        ret = cache_get_block(ktfs_global.cache, inode_pos, &blk);
        if (ret < 0) {
            lock_release(&ktfs_global.visit_cache);
            return ret;
        }

        actual_inode = (struct ktfs_inode *) (blk + inode_block_offset * sizeof(struct ktfs_inode));
        actual_inode->dindirect[which_dindirect] = inode->dindirect[which_dindirect];

        // release this cache block
        cache_release_block(ktfs_global.cache, blk, 1);
        lock_release(&ktfs_global.visit_cache);

        // initialize the newly allocated dindirect block
        pos = (data_block_0_num + inode->dindirect[which_dindirect]) * KTFS_BLKSZ;
        lock_acquire(&ktfs_global.visit_cache);
        ret = cache_get_block(ktfs_global.cache, pos, &blk);
        if (ret < 0) {
            lock_release(&ktfs_global.visit_cache);
            return ret;
        }
        memset(blk, 0, KTFS_BLKSZ);
        cache_release_block(ktfs_global.cache, blk, 1);
        lock_release(&ktfs_global.visit_cache);
    }

    // read the dindirect block to get the specified indirect block
    unsigned long long dindirect_lvl1_pos = (data_block_0_num + inode->dindirect[which_dindirect]) * KTFS_BLKSZ;
    lock_acquire(&ktfs_global.visit_cache);
    ret = cache_get_block(ktfs_global.cache, dindirect_lvl1_pos, &blk);
    if (ret < 0) {
        lock_release(&ktfs_global.visit_cache);
        return ret;
    }

    uint32_t * dindirect_blocks = (uint32_t *) blk;
    uint32_t indirect_blkno = dindirect_blocks[level1_index];

    // if the specified indirect block doesn't exist, allocate one
    if (level2_index == 0) {
        // first find a free block
        ret = find_free_block_bit(&indirect_blkno);
        if (ret < 0) {
            cache_release_block(ktfs_global.cache, blk, 0);
            lock_release(&ktfs_global.visit_cache);
            return ret;
        }
        // set the specified indirect block number
        dindirect_blocks[level1_index] = indirect_blkno;
        cache_release_block(ktfs_global.cache, blk, 1);
    }
    // release this cache block
    else {
        cache_release_block(ktfs_global.cache, blk, 0);
    }
    lock_release(&ktfs_global.visit_cache);

    // initialize the newly allocated indirect block
    if (level2_index == 0) {
        pos = (data_block_0_num + indirect_blkno) * KTFS_BLKSZ;
        lock_acquire(&ktfs_global.visit_cache);
        ret = cache_get_block(ktfs_global.cache, pos, &blk);
        if (ret < 0) {
            lock_release(&ktfs_global.visit_cache);
            return ret;
        }
        memset(blk, 0, KTFS_BLKSZ);
        cache_release_block(ktfs_global.cache, blk, 1);
        lock_release(&ktfs_global.visit_cache);
    }

    // write the block_number into the specified indirect block
    // read the specified indirect block
    unsigned long long lvl2_pos = (data_block_0_num + indirect_blkno) * KTFS_BLKSZ;
    lock_acquire(&ktfs_global.visit_cache);
    ret = cache_get_block(ktfs_global.cache, lvl2_pos, &blk);
    if (ret < 0) {
        lock_release(&ktfs_global.visit_cache);
        return ret;
    }
    // set the block_number
    uint32_t * lvl2_table = (uint32_t *) blk;
    lvl2_table[level2_index] = block_number;
    // release this cache block
    cache_release_block(ktfs_global.cache, blk, 1);
    lock_release(&ktfs_global.visit_cache);

    //ktfs_flush();

    return 0;
}

// helper function for ktfs_create
/*----------------------------------- find_free_inode -----------------------------------------
Description:
    Finds the lowest-numbered unused inode in the file system by scanning all directory entries
    in the root directory. This implementation assumes there is no inode bitmap, and instead
    relies on identifying in-use inodes through existing dentries. Inodes not referenced by any
    dentry are considered free.
Inputs:
    uint16_t *free_inode_num: Pointer to store the index of a free inode.
Outputs:
    Returns 0 on success and writes the free inode index to *free_inode_num.
    Returns -EINVAL if the input pointer is NULL.
    Returns -ENOINODEBLKS if no free inode is available.
Side Effects:
    None
----------------------------------------------------------------------------------------------*/
int find_free_inode(uint16_t * free_inode_num) {
    // check whether the input is valid
    if (free_inode_num == NULL) {
        return -EINVAL;
    }

    void * blk;
    int ret;
    unsigned long long pos;
    uint32_t i, j;

    // calculate the maximum number of inodes in the file system
    uint32_t max_inodes = ktfs_global.sb.inode_block_count * (KTFS_BLKSZ / sizeof(struct ktfs_inode));

    // initialize an array to track which inode numbers are already used
    uint8_t used[max_inodes];
    memset(used, 0, sizeof(used));

    uint32_t dentry_per_block = KTFS_BLKSZ / sizeof(struct ktfs_dir_entry);
    // number of inodes that are currently in use by the root directory
    uint32_t inode_in_use = ktfs_global.root_inode.size / sizeof(struct ktfs_dir_entry);
    // number of data blocks the root directory uses to store its directory entries
    uint32_t block_in_use = (inode_in_use + dentry_per_block - 1) / dentry_per_block;
    uint32_t data_block_0 = 1 + ktfs_global.sb.bitmap_block_count + ktfs_global.sb.inode_block_count;

    uint32_t blkno;
    uint32_t dentry_num;

    // iterate through each data block used by the root directory
    for (i = 0; i < block_in_use; i++) {
        // get the physical data block number for the i-th logical block in the root directory
        ret = get_data_block(&ktfs_global.root_inode, i, &blkno);
        if (ret < 0) {
            return ret;
        }

        // read the specified data block
        pos = (data_block_0 + blkno) * KTFS_BLKSZ;
        lock_acquire(&ktfs_global.visit_cache);
        ret = cache_get_block(ktfs_global.cache, pos, &blk);
        if (ret < 0) {
            lock_release(&ktfs_global.visit_cache);
            return ret;
        }

        struct ktfs_dir_entry * entries = (struct ktfs_dir_entry *) blk;
        // calculate how many entries are valid in this block
        // last block, maybe not full
        if (i == block_in_use - 1) {
            dentry_num = inode_in_use % dentry_per_block;
            // if perfectly full, use all entries
            if (dentry_num == 0) {
                dentry_num = dentry_per_block;
            }
        }
        // all previous blocks are completely full
        else {
            dentry_num = dentry_per_block;
        }

        // mark all in-use inode numbers found in this directory block as used
        for (j = 0; j < dentry_num; j++) {
            if (entries[j].inode < max_inodes) {
                used[entries[j].inode] = 1;
            }
        }

        // release this cache block
        cache_release_block(ktfs_global.cache, blk, 0);
        lock_release(&ktfs_global.visit_cache);
    }

    // root directory inode
    used[0] = 1;

    // scan for the first unused inode number
    for (i = 0; i < max_inodes; i++) {
        if (!used[i]) {
            *free_inode_num = i;
            return 0;
        }
    }

    // no free inode numbers found
    return -ENOINODEBLKS;
}

/*-------------------------------- safe_strlen ---------------------------------
Description:
    Calculates the length of a C string up to a maximum number of characters.
    Stops at the first null terminator or after maxlen characters.
Inputs:
    const char *s: Input string
    size_t maxlen: Maximum number of characters to check
Outputs:
    Returns the length of the string (not including the null terminator),
    or maxlen if no null terminator is found within the limit.
Side Effects:
    None
-------------------------------------------------------------------------------*/
size_t safe_strlen(const char *s, size_t maxlen) {
    size_t i;
    for (i = 0; i < maxlen; i++) {
        if (s[i] == '\0') {
            break;
        }
    }
    return i;
}

/*----------------------------------- ktfs_create -----------------------------------------
Description:
    Creates a new file named name of length 0 in the filesystem. This function will be used
    to create new zero-length files. It must validate that the name is valid and not already
    in-use (no duplicate named files). It must add a dentry and inode for the file to the
    filesystem image. The changes must immediately persist on the disk. This function won't
    open the file.
Inputs:
    const char * name: a string that is the name of the new file
Outputs:
    Returns 0 on success.
    Returns -EINVAL if name is invalid.
    Returns other negative error codes if cache access fails.
Side Effects:
    Modifies the inode table, root directory, and bitmap on disk.
------------------------------------------------------------------------------------------*/
int ktfs_create(const char * name)
{
    // check whether the input name is valid
    if (name == NULL) {
        return -EINVAL;
    }
    size_t len = safe_strlen(name, KTFS_MAX_FILENAME_LEN + 1);
    if (len <= 0 || len > KTFS_MAX_FILENAME_LEN) {
        return -EINVAL;
    }

    int ret, i, j;
    void *blk;
    unsigned long long pos;


    // first check for duplicate name

    uint32_t dentry_per_block = KTFS_BLKSZ / sizeof(struct ktfs_dir_entry);
    // the total number of existed files in this file system
    uint32_t inode_in_use = ktfs_global.root_inode.size / sizeof(struct ktfs_dir_entry);
    if (inode_in_use >= KTFS_MAX_INODES) {
        return -ENOINODEBLKS;
    }
    if (inode_in_use == 0) {
        goto create;
    }
    // the number of blocks that contain in-use inodes in the root directory
    uint32_t block_in_use = inode_in_use / dentry_per_block + 1;
    // the number of inodes contained in the last block
    uint32_t last_block_offset = inode_in_use % dentry_per_block;
    if (last_block_offset == 0) {
        block_in_use--;
    }
    // get the block number of data_block_0
    uint32_t data_block_0_num = 1 + ktfs_global.sb.bitmap_block_count + ktfs_global.sb.inode_block_count;

    uint32_t blkno;
    uint32_t dentry_num;

    // iterate through all blocks that contain in-use inodes in the root directory
    for (i = 0; i < block_in_use; i++) {
        // get the actual data block number from the root inode
        ret = get_data_block(&ktfs_global.root_inode, i, &blkno);
        // return an error code if fails
        if (ret < 0) {
            return ret;
        }

        // read the specified data block
        pos = (data_block_0_num + blkno) * KTFS_BLKSZ;
        lock_acquire(&ktfs_global.visit_cache);
        ret = cache_get_block(ktfs_global.cache, pos, &blk);
        // if the read fails
        if (ret < 0) {
            lock_release(&ktfs_global.visit_cache);
            return ret;
        }

        // if this is the last block, the total number of dentries stored in this block equals last_block_offset
        if (i == block_in_use - 1) {
            dentry_num = last_block_offset;
            if (dentry_num == 0 && block_in_use > 0) {
                dentry_num = dentry_per_block;
            }
        }
        else {
            dentry_num = dentry_per_block;
        }

        // iterate through all dentries stored in this block
        for (j = 0; j < dentry_num; j++) {
            // get the j-th dentry in this block
            struct ktfs_dir_entry * dentry = &((struct ktfs_dir_entry *)blk)[j];
            // if a duplicate name exists, return an error code
            if (strcmp(dentry->name, name) == 0) {
                cache_release_block(ktfs_global.cache, blk, 0);
                lock_release(&ktfs_global.visit_cache);
                return -EBUSY;
            }
        }

        // release this data block
        cache_release_block(ktfs_global.cache, blk, 0);
        lock_release(&ktfs_global.visit_cache);
    }


create:
    // if the name is valid, create a new file in the file system and write back to the disk

    // first find a free inode
    uint16_t free_inode;
    ret = find_free_inode(&free_inode);
    if (ret < 0) {
        return ret;
    }

    // no need to allocate a new block to store the new inode
    if (last_block_offset != 0) {
        // read the last block
        lock_acquire(&ktfs_global.visit_cache);
        ret = cache_get_block(ktfs_global.cache, pos, &blk);
        // if the read fails
        if (ret < 0) {
            lock_release(&ktfs_global.visit_cache);
            return ret;
        }

        // get the next dentry in this block
        struct ktfs_dir_entry * new_dentry = &((struct ktfs_dir_entry *) blk)[dentry_num];
        // ensure null-terminated
        strncpy(new_dentry->name, name, KTFS_MAX_FILENAME_LEN);
        new_dentry->name[KTFS_MAX_FILENAME_LEN] = '\0';
        new_dentry->inode = free_inode;

        // release this cache block
        cache_release_block(ktfs_global.cache, blk, 1);
        lock_release(&ktfs_global.visit_cache);
    }
    // need to allocate a new block to store the new inode
    else {
        // find a new unused data block
        uint32_t new_blkno;
        ret = find_free_block_bit(&new_blkno);
        if (ret < 0) {
            return ret;
        }
    
        // add the new block to the root inode's data block pointer
        ret = set_data_block(&ktfs_global.root_inode, block_in_use, new_blkno, 1);
        if (ret < 0) {
            return ret;
        }
    
        // get the physical location of the block and read
        pos = (data_block_0_num + new_blkno) * KTFS_BLKSZ;
        lock_acquire(&ktfs_global.visit_cache);
        ret = cache_get_block(ktfs_global.cache, pos, &blk);
        if (ret < 0) {
            lock_release(&ktfs_global.visit_cache);
            return ret;
        }
    
        // write the new dentry to the first position of the block
        struct ktfs_dir_entry * new_dentry = (struct ktfs_dir_entry *)blk;
        // ensure null-terminated
        strncpy(new_dentry->name, name, KTFS_MAX_FILENAME_LEN);
        new_dentry->name[KTFS_MAX_FILENAME_LEN] = '\0';
        new_dentry->inode = free_inode;
    
        // release this cache block
        cache_release_block(ktfs_global.cache, blk, 1);
        lock_release(&ktfs_global.visit_cache);
    }
    

    // increase the size in the root directory to add one inode
    ktfs_global.root_inode.size += sizeof(struct ktfs_dir_entry);
    // get the root directory inode
    // get the block number of inode_block_0
    uint32_t inode_block_0_num = 1 + ktfs_global.sb.bitmap_block_count;
    // find out which block the root directory inode is in
    uint16_t inodes_per_block = KTFS_BLKSZ / sizeof(struct ktfs_inode);
    uint16_t block_index = ktfs_global.sb.root_directory_inode / inodes_per_block;
    uint16_t block_offset = ktfs_global.sb.root_directory_inode % inodes_per_block;
    pos = (inode_block_0_num + block_index) * KTFS_BLKSZ;
    // read the root directory inode
    lock_acquire(&ktfs_global.visit_cache);
    ret = cache_get_block(ktfs_global.cache, pos, &blk);
    // if fails
    if (ret < 0) {
        lock_release(&ktfs_global.visit_cache);
        return ret;
    }
    // write the new size back to the disk
    struct ktfs_inode * root = (struct ktfs_inode *) (blk + block_offset * sizeof(struct ktfs_inode));
    root->size = ktfs_global.root_inode.size;
    // release this cache block
    cache_release_block(ktfs_global.cache, blk, 1);
    lock_release(&ktfs_global.visit_cache);


    // fill in the specified new inode
    // determine which inode block the inode is in
    block_index = free_inode / inodes_per_block;
    block_offset = free_inode % inodes_per_block;

    // read the specified new inode
    pos = (inode_block_0_num + block_index) * KTFS_BLKSZ;
    lock_acquire(&ktfs_global.visit_cache);
    ret = cache_get_block(ktfs_global.cache, pos, &blk);
    if (ret < 0) {
        lock_release(&ktfs_global.visit_cache);
        return ret;
    }
    // the new file has length 0
    struct ktfs_inode * inode_table = (struct ktfs_inode *) blk;
    struct ktfs_inode * new_inode = &inode_table[block_offset];
    memset(new_inode, 0, sizeof(struct ktfs_inode));
    new_inode->size = 0;

    // release this cache block
    cache_release_block(ktfs_global.cache, blk, 1);
    lock_release(&ktfs_global.visit_cache);

    //ktfs_flush();

    return 0;
}

// helper function for ktfs_delete
/*----------------------------------- free_block_bit ----------------------------------------
Description:
    Frees a data block in the filesystem by clearing the corresponding bit in the bitmap.
    This function is used during file deletion to mark a previously allocated block as
    available. The bitmap change is immediately persisted to disk.
Inputs:
    uint32_t blockno: The absolute block number to free in the filesystem.
Outputs:
    Returns 0 on success.
    Returns -EINVAL if the block number is outside the valid bitmap range.
    Returns other negative error codes if cache access fails.
Side Effects:
    Modifies the bitmap on disk to mark the specified block as free.
--------------------------------------------------------------------------------------------*/
int free_block_bit(uint32_t blockno) {
    void *blk;
    int ret;

    uint32_t bits_per_block = KTFS_BLKSZ * 8;
    uint32_t block_index = blockno / bits_per_block;
    uint32_t bit_index = blockno % bits_per_block;

    // check whether the block index is valid
    if (block_index >= ktfs_global.sb.bitmap_block_count) {
        return -EINVAL;
    }

    // read the bitmap block
    unsigned long long pos = (1 + block_index) * KTFS_BLKSZ;
    ret = cache_get_block(ktfs_global.cache, pos, &blk);
    if (ret < 0) {
        return ret;
    }

    // clear the bit
    uint8_t * bitmap = (uint8_t *) blk;
    bitmap[bit_index / 8] &= ~(1 << (bit_index % 8));

    // release the bitmap block
    cache_release_block(ktfs_global.cache, blk, 1);
    return 0;
}

// helper function for ktfs_delete
/*---------------------------- free_data_blocks ------------------------------
Description:
    Frees all data blocks associated with the given inode, including direct,
    indirect, and double indirect data blocks. Updates the bitmap and clears
    the contents of each freed block on disk. The inode's block pointers are
    reset to zero. All changes are persisted immediately.
Inputs:
    struct ktfs_inode *inode - pointer to the inode whose blocks are to be freed
Outputs:
    Returns 0 on success.
    Returns negative error codes if bitmap or cache operations fail.
Side Effects:
    Modifies the data block bitmap and clears data on disk. The inode's
    block pointers are reset.
----------------------------------------------------------------------------*/
int free_data_blocks(struct ktfs_inode * inode) {
    int ret;
    void *blk;
    uint32_t i, j;
    unsigned long long pos;

    uint32_t data_block_0_num = 1 + ktfs_global.sb.bitmap_block_count + ktfs_global.sb.inode_block_count;
    uint32_t ptrs_per_block = KTFS_BLKSZ / sizeof(uint32_t);

    // the total number of data blocks in use
    uint32_t total_block = (inode->size + KTFS_BLKSZ - 1) / KTFS_BLKSZ;

    // free all direct blocks
    for (i = 0; i < KTFS_NUM_DIRECT_DATA_BLOCKS; i++) {
        if (i < total_block) {
            // free the block in the bitmap
            ret = free_block_bit(data_block_0_num + inode->block[i]);
            if (ret < 0) {
                return ret;
            }

            // clear the block data on disk
            pos = (data_block_0_num + inode->block[i]) * KTFS_BLKSZ;
            ret = cache_get_block(ktfs_global.cache, pos, &blk);
            if (ret < 0) {
                return ret;
            }
            memset(blk, 0, KTFS_BLKSZ);
            cache_release_block(ktfs_global.cache, blk, 1);
            
            // reset the pointer in the inode
            inode->block[i] = 0;
        }
    }
    //ktfs_flush();

    // if complete
    if (total_block <= KTFS_NUM_DIRECT_DATA_BLOCKS) {
        return 0;
    }
    total_block -= KTFS_NUM_DIRECT_DATA_BLOCKS;

    // free the indirect block
    // read the indirect block
    void * indirect_blk;
    pos = (data_block_0_num + inode->indirect) * KTFS_BLKSZ;
    ret = cache_get_block(ktfs_global.cache, pos, &indirect_blk);
    if (ret < 0) {
        return ret;
    }

    // free all blocks pointed to by the indirect table
    uint32_t * table = (uint32_t *) indirect_blk;
    for (i = 0; i < ptrs_per_block; i++) {
        if (i < total_block) {
            // free the block in the bitmap
            ret = free_block_bit(data_block_0_num + table[i]);
            if (ret < 0) {
                cache_release_block(ktfs_global.cache, indirect_blk, 1);
                return ret;
            }

            // clear the block data on disk
            pos = (data_block_0_num + table[i]) * KTFS_BLKSZ;
            ret = cache_get_block(ktfs_global.cache, pos, &blk);
            if (ret < 0) {
                return ret;
            }
            memset(blk, 0, KTFS_BLKSZ);
            cache_release_block(ktfs_global.cache, blk, 1);
            //ktfs_flush();
        }
    }
    // clear the indirect block
    memset(indirect_blk, 0, KTFS_BLKSZ);
    cache_release_block(ktfs_global.cache, indirect_blk, 1);

    // free the indirect block in the bitmap
    ret = free_block_bit(data_block_0_num + inode->indirect);
    if (ret < 0) {
        return ret;
    }
    inode->indirect = 0;
    //ktfs_flush();

    // if complete
    if (total_block <= ptrs_per_block) {
        return 0;
    }
    total_block -= ptrs_per_block;

    // free all the doubly-indirect blocks
    void * dindirect_blk;
    for (int d = 0; d < KTFS_NUM_DINDIRECT_BLOCKS; d++) {
        if (total_block > 0) {
            // read the doubly-indirect block
            pos = (data_block_0_num + inode->dindirect[d]) * KTFS_BLKSZ;
            ret = cache_get_block(ktfs_global.cache, pos, &dindirect_blk);
            if (ret < 0) {
                return ret;
            }

            // iterate through first-level table entries
            uint32_t * lvl1_table = (uint32_t *) dindirect_blk;
            for (i = 0; i < ptrs_per_block; i++) {
                if (total_block > 0) {
                    // read the indirect block
                    unsigned long long inner_pos = (data_block_0_num + lvl1_table[i]) * KTFS_BLKSZ;
                    void * blk2;
                    ret = cache_get_block(ktfs_global.cache, inner_pos, &blk2);
                    if (ret < 0) {
                        cache_release_block(ktfs_global.cache, dindirect_blk, 1);
                        return ret;
                    }

                    // free all blocks pointed to by the second-level table
                    uint32_t * lvl2_table = (uint32_t *) blk2;
                    for (j = 0; j < ptrs_per_block; j++) {
                        if (total_block > 0) {
                            // free the block in the bitmap
                            ret = free_block_bit(data_block_0_num + lvl2_table[j]);
                            if (ret < 0) {
                                cache_release_block(ktfs_global.cache, blk2, 1);
                                return ret;
                            }

                            // clear block data on disk
                            pos = (data_block_0_num + lvl2_table[j]) * KTFS_BLKSZ;
                            ret = cache_get_block(ktfs_global.cache, pos, &blk);
                            if (ret < 0) {
                                cache_release_block(ktfs_global.cache, blk2, 1);
                                cache_release_block(ktfs_global.cache, dindirect_blk, 1);
                                return ret;
                            }
                            memset(blk, 0, KTFS_BLKSZ);
                            cache_release_block(ktfs_global.cache, blk, 1);

                            total_block--;
                            //ktfs_flush();
                        }
                    }

                    // clear and free the second-level pointer block
                    memset(blk2, 0, KTFS_BLKSZ);
                    cache_release_block(ktfs_global.cache, blk2, 1);
                    //ktfs_flush();

                    // free the indirect block in the bitmap
                    ret = free_block_bit(data_block_0_num + lvl1_table[i]);
                    if (ret < 0) {
                        cache_release_block(ktfs_global.cache, dindirect_blk, 1);
                        return ret;
                    }
                }
            }

            // clear and free the doubly-indirect block
            memset(dindirect_blk, 0, KTFS_BLKSZ);
            cache_release_block(ktfs_global.cache, dindirect_blk, 1);
            ret = free_block_bit(data_block_0_num + inode->dindirect[d]);
            if (ret < 0) {
                return ret;
            }

            inode->dindirect[d] = 0;
        }
    }
    //ktfs_flush();
    return 0;
}

/*------------------------------ ktfs_delete ---------------------------------
Description:
    Deletes a file named name from the filesystem. This function will be used
    to delete an existing file from the filesystem. It must free all data blocks
    and the inode associated with the file. It must remove the dentry associated
    with the file from the root directory. Note that dentries must be contiguous.
    The changes must immediately persist on the disk. This function should close
    the file if it is open.
Inputs:
    const char * name: a string that is the name of the file to delete
Outputs:
    Returns 0 on success.
    Returns -EINVAL if name is invalid.
    Returns other negative error codes if cache access fails.
Side Effects:
    Modifies the inode table, root directory, and bitmap on disk.
----------------------------------------------------------------------------*/
int ktfs_delete(const char * name) {
    // check whether the input name is valid
    if (name == NULL) {
        return -EINVAL;
    }
    size_t len = safe_strlen(name, KTFS_MAX_FILENAME_LEN + 1);
    if (len <= 0 || len > KTFS_MAX_FILENAME_LEN) {
        return -EINVAL;
    }

    //////// Multi-threading with locks
    // this function should close the file if it is open
    // search for the file in the opened_files array
    for (int a = 0; a < KTFS_MAX_OPEN_FILES; a++) {
        // if the correct file is found
        if (opened_files[a] != NULL && strncmp(name, opened_files[a]->dentry.name, sizeof(opened_files[a]->dentry.name)) == 0) {
            // mark file as closed
            opened_files[a]->isopen = 0;
            // free the iointf structure
            if (opened_files[a]->io.intf != NULL) {
                struct iointf * intf = (struct iointf *) opened_files[a]->io.intf;
                kfree(intf);
            }
            // free the ktfs_file structure
            kfree(opened_files[a]);
            // remove this file from the opened_files array
            opened_files[a] = NULL;
        }
    }

    int ret;
    void *blk;
    unsigned long long pos;
    uint32_t dentry_blkno, i, j;

    uint32_t dentry_per_block = KTFS_BLKSZ / sizeof(struct ktfs_dir_entry);
    uint32_t inode_in_use = ktfs_global.root_inode.size / sizeof(struct ktfs_dir_entry);
    uint32_t block_in_use = (inode_in_use + dentry_per_block - 1) / dentry_per_block;
    uint32_t data_block_0_num = 1 + ktfs_global.sb.bitmap_block_count + ktfs_global.sb.inode_block_count;

    struct ktfs_dir_entry * target_dentry = NULL;
    uint32_t target_dentry_block_idx = 0, target_dentry_index = 0;
    uint32_t entries_this_block;
    uint16_t inode_num;
    int found = 0;

    // find the matching dentry
    // iterate through all dentry blocks
    for (i = 0; i < block_in_use; i++) {
        // get the block number of the dentry block
        ret = get_data_block(&ktfs_global.root_inode, i, &dentry_blkno);
        if (ret < 0) {
            return ret;
        }

        // read the dentry block
        pos = (data_block_0_num + dentry_blkno) * KTFS_BLKSZ;
        lock_acquire(&ktfs_global.visit_cache);
        ret = cache_get_block(ktfs_global.cache, pos, &blk);
        if (ret < 0) {
            lock_release(&ktfs_global.visit_cache);
            return ret;
        }

        struct ktfs_dir_entry * entries = (struct ktfs_dir_entry *) blk;
        // calculate how many entries are valid in this block
        // last block, maybe not full
        if (i == block_in_use - 1) {
            entries_this_block = inode_in_use % dentry_per_block;
            // if perfectly full
            if (entries_this_block == 0) {
                entries_this_block = dentry_per_block;
            }
        } else {
            entries_this_block = dentry_per_block;
        }

        // if we find the correct file
        for (j = 0; j < entries_this_block; j++) {
            if (strncmp(entries[j].name, name, KTFS_MAX_FILENAME_LEN) == 0) {
                target_dentry = &entries[j];
                inode_num = target_dentry->inode;//inode的序号
                target_dentry_block_idx = i;
                target_dentry_index = j;
                // if found
                found = 1;
                break;
            }
        }

        cache_release_block(ktfs_global.cache, blk, 0);
        lock_release(&ktfs_global.visit_cache);

        if (found == 1) {
            break;
        }
    }

    // the file doesn't exist
    if (found == 0) {
        return -ENOENT;
    }

    // read the inode of this file from the disk
    uint32_t inode_block_0 = 1 + ktfs_global.sb.bitmap_block_count;
    uint32_t inodes_per_block = KTFS_BLKSZ / sizeof(struct ktfs_inode);
    uint32_t inode_block_idx = inode_num / inodes_per_block;
    uint32_t inode_block_offset = inode_num % inodes_per_block;

    pos = (inode_block_0 + inode_block_idx) * KTFS_BLKSZ;
    lock_acquire(&ktfs_global.visit_cache);
    ret = cache_get_block(ktfs_global.cache, pos, &blk);
    if (ret < 0) {
        lock_release(&ktfs_global.visit_cache);
        return ret;
    }

    struct ktfs_inode * inodes = (struct ktfs_inode *) blk;
    struct ktfs_inode * inode = &inodes[inode_block_offset];

    // release all data blocks pointed to by the inode
    ret = free_data_blocks(inode);
    if (ret < 0) {
        return ret;
    }

    // free this inode
    memset(inode, 0, sizeof(struct ktfs_inode));
    cache_release_block(ktfs_global.cache, blk, 1);
    lock_release(&ktfs_global.visit_cache);

    // remove the dentry
    // copy the last dentry to the current location
    uint32_t last_dentry_idx = inode_in_use - 1;
    uint32_t last_blk_idx = last_dentry_idx / dentry_per_block;
    uint32_t last_blk_offset = last_dentry_idx % dentry_per_block;

    // get the last dentry block number
    ret = get_data_block(&ktfs_global.root_inode, last_blk_idx, &dentry_blkno);
    if (ret < 0) {
        return ret;
    }
    // read the last dentry from the disk
    pos = (data_block_0_num + dentry_blkno) * KTFS_BLKSZ;
    lock_acquire(&ktfs_global.visit_cache);
    ret = cache_get_block(ktfs_global.cache, pos, &blk);
    if (ret < 0) {
        lock_release(&ktfs_global.visit_cache);
        return ret;
    }
    struct ktfs_dir_entry * last_block_dentries = (struct ktfs_dir_entry *) blk;
    // copy the last dentry
    struct ktfs_dir_entry last_entry = last_block_dentries[last_blk_offset];
    // clear the last dentry
    memset(&last_block_dentries[last_blk_offset], 0, sizeof(struct ktfs_dir_entry));

    // if the last dentry is the first one in the dentry block, free this dentry block
    if (last_blk_offset == 0) {
        memset(last_block_dentries, 0, KTFS_BLKSZ);
    }

    cache_release_block(ktfs_global.cache, blk, 1);
    lock_release(&ktfs_global.visit_cache);

    uint32_t root_idx = ktfs_global.sb.root_directory_inode / inodes_per_block;
    uint32_t root_off = ktfs_global.sb.root_directory_inode % inodes_per_block;
    unsigned long long root_pos = (inode_block_0 + root_idx) * KTFS_BLKSZ;
    // if the last dentry is the first one in the dentry block, free this dentry block
    if (last_blk_offset == 0) {
        // clear the bitmap
        ret = free_block_bit(data_block_0_num + dentry_blkno);
        if (ret < 0) {
            return ret;
        }
        // clear this dentry block in the root inode
        lock_acquire(&ktfs_global.visit_cache);
        ret = cache_get_block(ktfs_global.cache, root_pos, &blk);
        if (ret < 0) {
            lock_release(&ktfs_global.visit_cache);
            return ret;
        }
        ((struct ktfs_inode *)blk)[root_off].block[last_blk_idx] = 0;
        cache_release_block(ktfs_global.cache, blk, 1);
        lock_release(&ktfs_global.visit_cache);
    }


    // copy the last entry to the target position
    // only when the deleted file is not the last one, we need to copy
    if (strncmp(name, last_entry.name, KTFS_MAX_FILENAME_LEN + 1) != 0) {
        // get the target block from the disk
        ret = get_data_block(&ktfs_global.root_inode, target_dentry_block_idx, &dentry_blkno);
        if (ret < 0) {
            return ret;
        }
        pos = (data_block_0_num + dentry_blkno) * KTFS_BLKSZ;
        lock_acquire(&ktfs_global.visit_cache);
        ret = cache_get_block(ktfs_global.cache, pos, &blk);
        if (ret < 0) {
            lock_release(&ktfs_global.visit_cache);
            return ret;
        }
        // copy
        ((struct ktfs_dir_entry *)blk)[target_dentry_index] = last_entry;
        cache_release_block(ktfs_global.cache, blk, 1);
        lock_release(&ktfs_global.visit_cache);
    }

    // update root inode size
    ktfs_global.root_inode.size -= sizeof(struct ktfs_dir_entry);

    // write the new root inode size back to the disk
    lock_acquire(&ktfs_global.visit_cache);
    ret = cache_get_block(ktfs_global.cache, root_pos, &blk);
    if (ret < 0) {
        lock_release(&ktfs_global.visit_cache);
        return ret;
    }
    ((struct ktfs_inode *)blk)[root_off].size = ktfs_global.root_inode.size;
    cache_release_block(ktfs_global.cache, blk, 1);
    lock_release(&ktfs_global.visit_cache);

    //ktfs_flush();
    return 0;
}
