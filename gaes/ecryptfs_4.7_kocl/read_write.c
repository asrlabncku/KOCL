/**
 * eCryptfs: Linux filesystem encryption layer
 *
 * Copyright (C) 2007 International Business Machines Corp.
 *   Author(s): Michael A. Halcrow <mahalcro@us.ibm.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA
 * 02111-1307, USA.
 */

#include <linux/fs.h>
#include <linux/pagemap.h>
#include <linux/slab.h>
#include "ecryptfs_kernel.h"

/**
 * ecryptfs_write_lower
 * @ecryptfs_inode: The eCryptfs inode
 * @data: Data to write
 * @offset: Byte offset in the lower file to which to write the data
 * @size: Number of bytes from @data to write at @offset in the lower
 *        file
 *
 * Write data to the lower file.
 *
 * Returns bytes written on success; less than zero on error
 */
int ecryptfs_write_lower(struct inode *ecryptfs_inode, char *data,
			 loff_t offset, size_t size)
{
	struct file *lower_file;
	ssize_t rc;

	lower_file = ecryptfs_inode_to_private(ecryptfs_inode)->lower_file;
	if (!lower_file)
		return -EIO;
	rc = kernel_write(lower_file, data, size, offset);
	mark_inode_dirty_sync(ecryptfs_inode);
	return rc;
}

/**
 * ecryptfs_write_lower_page_segment
 * @ecryptfs_inode: The eCryptfs inode
 * @page_for_lower: The page containing the data to be written to the
 *                  lower file
 * @offset_in_page: The offset in the @page_for_lower from which to
 *                  start writing the data
 * @size: The amount of data from @page_for_lower to write to the
 *        lower file
 *
 * Determines the byte offset in the file for the given page and
 * offset within the page, maps the page, and makes the call to write
 * the contents of @page_for_lower to the lower inode.
 *
 * Returns zero on success; non-zero otherwise
 */
int ecryptfs_write_lower_page_segment(struct inode *ecryptfs_inode,
				      struct page *page_for_lower,
				      size_t offset_in_page, size_t size)
{
	char *virt;
	loff_t offset;
	int rc;

	offset = ((((loff_t)page_for_lower->index) << PAGE_SHIFT)
		  + offset_in_page);
	virt = kmap(page_for_lower);
	rc = ecryptfs_write_lower(ecryptfs_inode, virt, offset, size);
	if (rc > 0)
		rc = 0;
	kunmap(page_for_lower);
	return rc;
}

/**
 * ecryptfs_write
 * @ecryptfs_inode: The eCryptfs file into which to write
 * @data: Virtual address where data to write is located
 * @offset: Offset in the eCryptfs file at which to begin writing the
 *          data from @data
 * @size: The number of bytes to write from @data
 *
 * Write an arbitrary amount of data to an arbitrary location in the
 * eCryptfs inode page cache. This is done on a page-by-page, and then
 * by an extent-by-extent, basis; individual extents are encrypted and
 * written to the lower page cache (via VFS writes). This function
 * takes care of all the address translation to locations in the lower
 * filesystem; it also handles truncate events, writing out zeros
 * where necessary.
 *
 * Returns zero on success; non-zero otherwise
 */
int ecryptfs_write(struct inode *ecryptfs_inode, char *data, loff_t offset,
		   size_t size)
{
	struct page *ecryptfs_page;
	struct ecryptfs_crypt_stat *crypt_stat;
	char *ecryptfs_page_virt;
	loff_t ecryptfs_file_size = i_size_read(ecryptfs_inode);
	loff_t data_offset = 0;
	loff_t pos;
	int rc = 0;

   // printk("[g-ecryptfs] Info: write size %lu \n", size); 

	crypt_stat = &ecryptfs_inode_to_private(ecryptfs_inode)->crypt_stat;
	/*
	 * if we are writing beyond current size, then start pos
	 * at the current size - we'll fill in zeros from there.
	 */
	if (offset > ecryptfs_file_size)
		pos = ecryptfs_file_size;
	else
		pos = offset;
	while (pos < (offset + size)) {
		pgoff_t ecryptfs_page_idx = (pos >> PAGE_SHIFT);
		size_t start_offset_in_page = (pos & ~PAGE_MASK);
		size_t num_bytes = (PAGE_SIZE - start_offset_in_page);
		loff_t total_remaining_bytes = ((offset + size) - pos);

		if (fatal_signal_pending(current)) {
			rc = -EINTR;
			break;
		}

		if (num_bytes > total_remaining_bytes)
			num_bytes = total_remaining_bytes;
		if (pos < offset) {
			/* remaining zeros to write, up to destination offset */
			loff_t total_remaining_zeros = (offset - pos);

			if (num_bytes > total_remaining_zeros)
				num_bytes = total_remaining_zeros;
		}
		ecryptfs_page = ecryptfs_get_locked_page(ecryptfs_inode,
							 ecryptfs_page_idx);
		if (IS_ERR(ecryptfs_page)) {
			rc = PTR_ERR(ecryptfs_page);
			printk(KERN_ERR "%s: Error getting page at "
			       "index [%ld] from eCryptfs inode "
			       "mapping; rc = [%d]\n", __func__,
			       ecryptfs_page_idx, rc);
			goto out;
		}
		ecryptfs_page_virt = kmap_atomic(ecryptfs_page);

		/*
		 * pos: where we're now writing, offset: where the request was
		 * If current pos is before request, we are filling zeros
		 * If we are at or beyond request, we are writing the *data*
		 * If we're in a fresh page beyond eof, zero it in either case
		 */
		if (pos < offset || !start_offset_in_page) {
			/* We are extending past the previous end of the file.
			 * Fill in zero values to the end of the page */
			memset(((char *)ecryptfs_page_virt
				+ start_offset_in_page), 0,
				PAGE_SIZE - start_offset_in_page);
		}

		/* pos >= offset, we are now writing the data request */
		if (pos >= offset) {
			memcpy(((char *)ecryptfs_page_virt
				+ start_offset_in_page),
			       (data + data_offset), num_bytes);
			data_offset += num_bytes;
		}
		kunmap_atomic(ecryptfs_page_virt);
		flush_dcache_page(ecryptfs_page);
		SetPageUptodate(ecryptfs_page);
		unlock_page(ecryptfs_page);
		if (crypt_stat->flags & ECRYPTFS_ENCRYPTED)
			rc = ecryptfs_encrypt_page(ecryptfs_page);
		else
			rc = ecryptfs_write_lower_page_segment(ecryptfs_inode,
						ecryptfs_page,
						start_offset_in_page,
						data_offset);
		put_page(ecryptfs_page);
		if (rc) {
			printk(KERN_ERR "%s: Error encrypting "
			       "page; rc = [%d]\n", __func__, rc);
			goto out;
		}
		pos += num_bytes;
	}
	if (pos > ecryptfs_file_size) {
		i_size_write(ecryptfs_inode, pos);
		if (crypt_stat->flags & ECRYPTFS_ENCRYPTED) {
			int rc2;

			rc2 = ecryptfs_write_inode_size_to_metadata(
								ecryptfs_inode);
			if (rc2) {
				printk(KERN_ERR	"Problem with "
				       "ecryptfs_write_inode_size_to_metadata; "
				       "rc = [%d]\n", rc2);
				if (!rc)
					rc = rc2;
				goto out;
			}
		}
	}
out:
	return rc;
}

int ecryptfs_write2(struct file *file, struct inode *ecryptfs_inode, char *data, loff_t offset,
		   size_t size)
{
	struct page *ecryptfs_page;
	struct page **pgs;
	struct ecryptfs_crypt_stat *crypt_stat;
	char *ecryptfs_page_virt;
	loff_t ecryptfs_file_size = i_size_read(ecryptfs_inode);
	loff_t data_offset = 0;
	loff_t pos;
	int nrpgs = size/PAGE_SIZE;
	int rc = 0;
	int i=0;
	struct address_space *mapping = file->f_mapping;
	unsigned int flags = 0;

	if (size&(PAGE_SIZE-1))//表示size不足4KB
	    nrpgs++;

	pgs = kmalloc(nrpgs*sizeof(struct page*), GFP_KERNEL);
	if (!pgs) {
	    printk("[g-ecryptfs] Error: allocate pages failed\n");
	    rc = -ENOMEM;
	    goto out;
	}

	crypt_stat = &ecryptfs_inode_to_private(ecryptfs_inode)->crypt_stat;
	/*
	 * if we are writing beyond current size, then start pos
	 * at the current size - we'll fill in zeros from there.
	 */
	if (offset > ecryptfs_file_size)
		pos = ecryptfs_file_size;
	else
		pos = offset;
	while (pos < (offset + size)) {
		pgoff_t ecryptfs_page_idx = (pos >> PAGE_SHIFT);
		size_t start_offset_in_page = (pos & ~PAGE_MASK);
		size_t num_bytes = (PAGE_SIZE - start_offset_in_page);
		size_t total_remaining_bytes = ((offset + size) - pos);

		if (num_bytes > total_remaining_bytes)
			num_bytes = total_remaining_bytes;
		if (pos < offset) {
			/* remaining zeros to write, up to destination offset */
			size_t total_remaining_zeros = (offset - pos);

			if (num_bytes > total_remaining_zeros)
				num_bytes = total_remaining_zeros;
		}
		/*ecryptfs_page = ecryptfs_get_locked_page(ecryptfs_inode,
							 ecryptfs_page_idx);//這裡會呼叫read_mapping_page()->mapping->a_ops->readpage;*/
		ecryptfs_page = grab_cache_page_write_begin(mapping, ecryptfs_page_idx, flags);
		if (IS_ERR(ecryptfs_page)) {
			rc = PTR_ERR(ecryptfs_page);
			printk(KERN_ERR "%s: Error getting page at "
			       "index [%ld] from eCryptfs inode "
			       "mapping; rc = [%d]\n", __func__,
			       ecryptfs_page_idx, rc);
			goto out;
		}
		ecryptfs_page_virt = kmap(ecryptfs_page);

		/*
		 * pos: where we're now writing, offset: where the request was
		 * If current pos is before request, we are filling zeros
		 * If we are at or beyond request, we are writing the *data*
		 * If we're in a fresh page beyond eof, zero it in either case
		 */
		if (pos < offset || !start_offset_in_page) {
			/* We are extending past the previous end of the file.
			 * Fill in zero values to the end of the page */
			memset(((char *)ecryptfs_page_virt
				+ start_offset_in_page), 0,
				PAGE_SIZE - start_offset_in_page);
		}

		/* pos >= offset, we are now writing the data request */
		if (pos >= offset) {
    	copy_from_user(((char *)ecryptfs_page_virt
	  				+ start_offset_in_page),
			       (data + data_offset), num_bytes);
			data_offset += num_bytes;
		}
		kunmap(ecryptfs_page);
		flush_dcache_page(ecryptfs_page);
		SetPageUptodate(ecryptfs_page);
		unlock_page(ecryptfs_page);
		if (crypt_stat->flags & ECRYPTFS_ENCRYPTED) {
		    pgs[i++] = ecryptfs_page;
		    /* rc = ecryptfs_encrypt_page(ecryptfs_page); */
		}
		else {
		    rc = ecryptfs_write_lower_page_segment(ecryptfs_inode,
						ecryptfs_page,
						start_offset_in_page,
						data_offset);
		    put_page(ecryptfs_page);
		    if (rc) {
			printk(KERN_ERR "%s: Error encrypting "
			       "page; rc = [%d]\n", __func__, rc);
			goto out;
		    }
		}
		pos += num_bytes;
	}

	if (crypt_stat->flags & ECRYPTFS_ENCRYPTED) {
	    rc = ecryptfs_encrypt_pages2(pgs, nrpgs);
	    for (i=0; i<nrpgs; i++)
		        put_page(pgs[i]);
	    kfree(pgs);
	}
	
	if ((offset + size) > ecryptfs_file_size) {
		i_size_write(ecryptfs_inode, (offset + size));
		if (crypt_stat->flags & ECRYPTFS_ENCRYPTED) {
			rc = ecryptfs_write_inode_size_to_metadata(
								ecryptfs_inode);
			if (rc) {
				printk(KERN_ERR	"Problem with "
				       "ecryptfs_write_inode_size_to_metadata; "
				       "rc = [%d]\n", rc);
				goto out;
			}
		}
	}
out:
	return rc;
}


/**
 * ecryptfs_read_lower
 * @data: The read data is stored here by this function
 * @offset: Byte offset in the lower file from which to read the data
 * @size: Number of bytes to read from @offset of the lower file and
 *        store into @data
 * @ecryptfs_inode: The eCryptfs inode
 *
 * Read @size bytes of data at byte offset @offset from the lower
 * inode into memory location @data.
 *
 * Returns bytes read on success; 0 on EOF; less than zero on error
 */
int ecryptfs_read_lower(char *data, loff_t offset, size_t size,
			struct inode *ecryptfs_inode)
{
	struct file *lower_file;
	lower_file = ecryptfs_inode_to_private(ecryptfs_inode)->lower_file;
	if (!lower_file)
		return -EIO;
	return kernel_read(lower_file, offset, data, size);
}

/**
 * ecryptfs_read_lower_page_segment
 * @page_for_ecryptfs: The page into which data for eCryptfs will be
 *                     written
 * @offset_in_page: Offset in @page_for_ecryptfs from which to start
 *                  writing
 * @size: The number of bytes to write into @page_for_ecryptfs
 * @ecryptfs_inode: The eCryptfs inode
 *
 * Determines the byte offset in the file for the given page and
 * offset within the page, maps the page, and makes the call to read
 * the contents of @page_for_ecryptfs from the lower inode.
 *
 * Returns zero on success; non-zero otherwise
 */
int ecryptfs_read_lower_page_segment(struct page *page_for_ecryptfs,
				     pgoff_t page_index,
				     size_t offset_in_page, size_t size,
				     struct inode *ecryptfs_inode)
{
	char *virt;
	loff_t offset;
	int rc;

	offset = ((((loff_t)page_index) << PAGE_SHIFT) + offset_in_page);
	virt = kmap(page_for_ecryptfs);
	rc = ecryptfs_read_lower(virt, offset, size, ecryptfs_inode);
	if (rc > 0)
		rc = 0;
	kunmap(page_for_ecryptfs);
	flush_dcache_page(page_for_ecryptfs);
	return rc;
}

/**
 * ecryptfs_read
 * @data: The virtual address into which to write the data read (and
 *        possibly decrypted) from the lower file
 * @offset: The offset in the decrypted view of the file from which to
 *          read into @data
 * @size: The number of bytes to read into @data
 * @ecryptfs_file: The eCryptfs file from which to read
 *
 * Read an arbitrary amount of data from an arbitrary location in the
 * eCryptfs page cache. This is done on an extent-by-extent basis;
 * individual extents are decrypted and read from the lower page
 * cache (via VFS reads). This function takes care of all the
 * address translation to locations in the lower filesystem.
 *
 * Returns zero on success; non-zero otherwise
 */
int ecryptfs_read2(struct file *ecryptfs_file, char *data, loff_t offset, size_t size)
{
	struct inode *ecryptfs_inode = ecryptfs_file->f_path.dentry->d_inode;
	struct ecryptfs_crypt_stat *crypt_stat =
	    		&ecryptfs_inode_to_private(ecryptfs_inode)->crypt_stat;
	struct address_space *mapping = ecryptfs_file->f_mapping;
	struct page *ecryptfs_page;
	char *ecryptfs_page_virt;
	loff_t ecryptfs_file_size = i_size_read(ecryptfs_inode);
	loff_t data_offset = 0;
	loff_t pos;
	int rc = 0,rc2=0, written=0;
    int nr_pages = 0;
	struct page **pgs = NULL;
	int nodec = 0;	//no decryption needed flag
	unsigned int page_idx = 0;
	struct page * page;
    
	 
	if( size > ecryptfs_file_size )
			size = ecryptfs_file_size;

    nr_pages = (size + PAGE_SIZE - 1) >> PAGE_SHIFT;//算出有多少page

	if (!crypt_stat
	    || !(crypt_stat->flags & ECRYPTFS_ENCRYPTED)	    
	    || (crypt_stat->flags & ECRYPTFS_VIEW_AS_ENCRYPTED)) {
	    nodec = 1;
	}
	
	if (!nodec) {
		pgs = (struct page **)kmalloc(nr_pages*sizeof(struct page*), GFP_KERNEL);		
	    if (!pgs) {
		   return -EFAULT;
	    }
	}

	for (page_idx = 0; page_idx < nr_pages; page_idx++) {
	     page = grab_cache_page(mapping, page_idx);
 			if (!page){
				 printk("[g-eCryptfs] INFO: cannot grab_cache_page !\n");
				 goto out;
			 } 					   
		   if (nodec)
		       rc |= mapping->a_ops->readpage(ecryptfs_file, page);//這邊會去判斷如果不需要decrypt就直接從lower read
	   
		pgs[page_idx] = page;
	}

    if (!nodec) {
	    rc = ecryptfs_decrypt_pages(pgs, nr_pages);//decrypt pages 並把資料從disk填入page	    
	}
 
    //printk("[g-ecryptfs] decrypt pages: %u, size: %lu , nodec: %u \n", nr_pages, size, nodec);
    
    /* After the decrypt, we copy the data to userspace */
	if ( offset  >= ecryptfs_file_size) {
		//rc2 = -EINVAL;
		printk("Attempt to read data past the end of the "
				"file; offset = [%lld]; size = [%td]; "
		       "ecryptfs_file_size = [%lld]\n",
		        offset, size, ecryptfs_file_size);
		offset= ecryptfs_file_size;
		goto out;
	}

	pos = offset;
	page_idx = 0;

	while (pos < (offset + size)) {
		pgoff_t ecryptfs_page_idx = (pos >> PAGE_SHIFT);
		size_t start_offset_in_page = (pos & ~PAGE_MASK);
		size_t num_bytes = (PAGE_SIZE - start_offset_in_page);
		size_t total_remaining_bytes = ((offset + size) - pos);

		if (num_bytes > total_remaining_bytes)
			num_bytes = total_remaining_bytes;
		
		ecryptfs_page = pgs[page_idx];
		
		if (IS_ERR(ecryptfs_page)) {
			rc2 = PTR_ERR(ecryptfs_page);
			printk(KERN_ERR "%s: Error getting page at "
			       "index [%ld] from eCryptfs inode "
			       "mapping; rc = [%d]\n", __func__,
			       ecryptfs_page_idx, rc2);
			goto out;
		}
		ecryptfs_page_virt = kmap_atomic(ecryptfs_page);
		copy_to_user((data + data_offset),
		       		((char *)ecryptfs_page_virt + start_offset_in_page),
		       		  num_bytes);
		kunmap_atomic(ecryptfs_page_virt);
		flush_dcache_page(ecryptfs_page);
		 
		 if (rc)
		          ClearPageUptodate(pgs[page_idx]);
		     else
		          SetPageUptodate(pgs[page_idx]);
		
		unlock_page(ecryptfs_page);
		put_page(ecryptfs_page);
		pos += num_bytes;
		written += num_bytes;
		data_offset += num_bytes;
		page_idx++;
	}
  //  printk("[g-ecryptfs] ecryptfs_read2:%lld \n",pos);
	 kfree(pgs);
out:
    file_accessed(ecryptfs_file);
	return written;
}

// int ecryptfs_read3(struct file *ecryptfs_file, char *data, loff_t *ppos, size_t size)
// {
// 	struct address_space *mapping = filp->f_mapping;
// 	struct inode *inode = mapping->host;
// 	struct file_ra_state *ra = &filp->f_ra;
// 	pgoff_t index;
// 	pgoff_t last_index;
// 	pgoff_t prev_index;
// 	unsigned long offset;      /* offset into pagecache page */
// 	unsigned int prev_offset;
// 	int error = 0;

// 	index = *ppos >> PAGE_SHIFT;
// 	prev_index = ra->prev_pos >> PAGE_SHIFT;
// 	prev_offset = ra->prev_pos & (PAGE_SIZE-1);
// 	last_index = (*ppos + iter->count + PAGE_SIZE-1) >> PAGE_SHIFT;
// 	offset = *ppos & ~PAGE_MASK;

// 	for (;;) {
// 		struct page *page;
// 		pgoff_t end_index;
// 		loff_t isize;
// 		unsigned long nr, ret;

// 		cond_resched();
// find_page:
// 		page = find_get_page(mapping, index);
// 		if (!page) {
// 			page_cache_sync_readahead(mapping,
// 					ra, filp,
// 					index, last_index - index);
// 			page = find_get_page(mapping, index);
// 			if (unlikely(page == NULL))
// 				goto no_cached_page;
// 		}
// 		if (PageReadahead(page)) {
// 			page_cache_async_readahead(mapping,
// 					ra, filp, page,
// 					index, last_index - index);
// 		}
// 		if (!PageUptodate(page)) {
// 			/*
// 			 * See comment in do_read_cache_page on why
// 			 * wait_on_page_locked is used to avoid unnecessarily
// 			 * serialisations and why it's safe.
// 			 */
// 			wait_on_page_locked_killable(page);
// 			if (PageUptodate(page))
// 				goto page_ok;

// 			if (inode->i_blkbits == PAGE_SHIFT ||
// 					!mapping->a_ops->is_partially_uptodate)
// 				goto page_not_up_to_date;
// 			if (!trylock_page(page))
// 				goto page_not_up_to_date;
// 			/* Did it get truncated before we got the lock? */
// 			if (!page->mapping)
// 				goto page_not_up_to_date_locked;
// 			if (!mapping->a_ops->is_partially_uptodate(page,
// 							offset, iter->count))
// 				goto page_not_up_to_date_locked;
// 			unlock_page(page);
// 		}
// page_ok:
// 		/*
// 		 * i_size must be checked after we know the page is Uptodate.
// 		 *
// 		 * Checking i_size after the check allows us to calculate
// 		 * the correct value for "nr", which means the zero-filled
// 		 * part of the page is not copied back to userspace (unless
// 		 * another truncate extends the file - this is desired though).
// 		 */

// 		isize = i_size_read(inode);
// 		end_index = (isize - 1) >> PAGE_SHIFT;
// 		if (unlikely(!isize || index > end_index)) {
// 			put_page(page);
// 			goto out;
// 		}

// 		/* nr is the maximum number of bytes to copy from this page */
// 		nr = PAGE_SIZE;
// 		if (index == end_index) {
// 			nr = ((isize - 1) & ~PAGE_MASK) + 1;
// 			if (nr <= offset) {
// 				put_page(page);
// 				goto out;
// 			}
// 		}
// 		nr = nr - offset;

// 		/* If users can be writing to this page using arbitrary
// 		 * virtual addresses, take care about potential aliasing
// 		 * before reading the page on the kernel side.
// 		 */
// 		if (mapping_writably_mapped(mapping))
// 			flush_dcache_page(page);

// 		/*
// 		 * When a sequential read accesses a page several times,
// 		 * only mark it as accessed the first time.
// 		 */
// 		if (prev_index != index || offset != prev_offset)
// 			mark_page_accessed(page);
// 		prev_index = index;

// 		/*
// 		 * Ok, we have the page, and it's up-to-date, so
// 		 * now we can copy it to user space...
// 		 */

// 		ret = copy_page_to_iter(page, offset, nr, iter);
// 		offset += ret;
// 		index += offset >> PAGE_SHIFT;
// 		offset &= ~PAGE_MASK;
// 		prev_offset = offset;

// 		put_page(page);
// 		written += ret;
// 		if (!iov_iter_count(iter))
// 			goto out;
// 		if (ret < nr) {
// 			error = -EFAULT;
// 			goto out;
// 		}
// 		continue;

// page_not_up_to_date:
// 		/* Get exclusive access to the page ... */
// 		error = lock_page_killable(page);
// 		if (unlikely(error))
// 			goto readpage_error;

// page_not_up_to_date_locked:
// 		/* Did it get truncated before we got the lock? */
// 		if (!page->mapping) {
// 			unlock_page(page);
// 			put_page(page);
// 			continue;
// 		}

// 		/* Did somebody else fill it already? */
// 		if (PageUptodate(page)) {
// 			unlock_page(page);
// 			goto page_ok;
// 		}

// readpage:
// 		/*
// 		 * A previous I/O error may have been due to temporary
// 		 * failures, eg. multipath errors.
// 		 * PG_error will be set again if readpage fails.
// 		 */
// 		ClearPageError(page);
// 		/* Start the actual read. The read will unlock the page. */
// 		error = mapping->a_ops->readpage(filp, page);

// 		if (unlikely(error)) {
// 			if (error == AOP_TRUNCATED_PAGE) {
// 				put_page(page);
// 				error = 0;
// 				goto find_page;
// 			}
// 			goto readpage_error;
// 		}

// 		if (!PageUptodate(page)) {
// 			error = lock_page_killable(page);
// 			if (unlikely(error))
// 				goto readpage_error;
// 			if (!PageUptodate(page)) {
// 				if (page->mapping == NULL) {
// 					/*
// 					 * invalidate_mapping_pages got it
// 					 */
// 					unlock_page(page);
// 					put_page(page);
// 					goto find_page;
// 				}
// 				unlock_page(page);
// 				shrink_readahead_size_eio(filp, ra);
// 				error = -EIO;
// 				goto readpage_error;
// 			}
// 			unlock_page(page);
// 		}

// 		goto page_ok;

// readpage_error:
// 		/* UHHUH! A synchronous read error occurred. Report it */
// 		put_page(page);
// 		goto out;

// no_cached_page:
// 		/*
// 		 * Ok, it wasn't cached, so we need to create a new
// 		 * page..
// 		 */
// 		page = page_cache_alloc_cold(mapping);
// 		if (!page) {
// 			error = -ENOMEM;
// 			goto out;
// 		}
// 		error = add_to_page_cache_lru(page, mapping, index,
// 				mapping_gfp_constraint(mapping, GFP_KERNEL));
// 		if (error) {
// 			put_page(page);
// 			if (error == -EEXIST) {
// 				error = 0;
// 				goto find_page;
// 			}
// 			goto out;
// 		}
// 		goto readpage;
// 	}

// out:
// 	ra->prev_pos = prev_index;
// 	ra->prev_pos <<= PAGE_SHIFT;
// 	ra->prev_pos |= prev_offset;

// 	*ppos = ((loff_t)index << PAGE_SHIFT) + offset;
// 	file_accessed(filp);
// 	return written ? written : error;
// }
