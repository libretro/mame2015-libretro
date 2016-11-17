// license:BSD-3-Clause
// copyright-holders:Aaron Giles
/***************************************************************************

    corealloc.c

    Memory allocation helpers for the helper library.

***************************************************************************/

#include "corealloc.h"
#include "osdcore.h"


//**************************************************************************
//  DEBUGGING
//**************************************************************************

//**************************************************************************
//  CONSTANTS
//**************************************************************************

// number of memory_entries to allocate in a block
const int memory_block_alloc_chunk = 256;



//**************************************************************************
//  TYPE DEFINITIONS
//**************************************************************************

// this struct is allocated in pools to track memory allocations
// it must be a POD type!!
class memory_entry
{
public:
	// internal state
	memory_entry *      m_next;             // link to the next entry
	memory_entry *      m_prev;             // link to the previous entry
	size_t              m_size;             // size of the allocation (not including this header)
	void *              m_base;             // base of the allocation
	const char *        m_file;             // file the allocation was made from
	int                 m_line;             // line number within that file
	UINT64              m_id;               // unique id
	bool                m_array;            // array?

	// hashing prime number
	static const int    k_hash_prime = 6151;

	// global state
	static UINT64       s_curid;            // current ID
	static osd_lock *   s_lock;             // lock for managing the list
	static bool         s_lock_alloc;       // set to true temporarily during lock allocation
	static bool         s_tracking;         // set to true when tracking is live
	static memory_entry *s_hash[k_hash_prime];// hash table based on pointer
	static memory_entry *s_freehead;        // pointer to the head of the free list

	// static helpers
	static memory_entry *allocate(size_t size, void *base, const char *file, int line, bool array);
	static memory_entry *find(void *ptr);
	static void release(memory_entry *entry, const char *file, int line);
	static void report_unfreed(UINT64 start);

private:
	static void acquire_lock();
};



//**************************************************************************
//  GLOBALS
//**************************************************************************

// dummy zeromem object
const zeromem_t zeromem = { };

// globals for memory_entry
UINT64 memory_entry::s_curid = 1;
osd_lock *memory_entry::s_lock = NULL;
bool memory_entry::s_lock_alloc = false;
bool memory_entry::s_tracking = false;
memory_entry *memory_entry::s_hash[memory_entry::k_hash_prime] = { NULL };
memory_entry *memory_entry::s_freehead = NULL;

//**************************************************************************
//  OPERATOR REPLACEMENTS
//**************************************************************************

#ifndef NO_MEM_TRACKING

// standard new/delete operators (try to avoid using)
void *operator new(std::size_t size) throw (std::bad_alloc) { return malloc_file_line(size, NULL, 0, false, true, false); }
void *operator new[](std::size_t size) throw (std::bad_alloc) { return malloc_file_line(size, NULL, 0, true, true, false); }
void operator delete(void *ptr) throw() { if (ptr != NULL) free_file_line(ptr, NULL, 0, false); }
void operator delete[](void *ptr) throw() { if (ptr != NULL) free_file_line(ptr, NULL, 0, true); }

void* operator new(std::size_t size,const std::nothrow_t&) throw() { return malloc_file_line(size, NULL, 0, false, false, false); }
void* operator new[](std::size_t size, const std::nothrow_t&) throw() { return malloc_file_line(size, NULL, 0, true, false, false); }
void operator delete(void* ptr, const std::nothrow_t&) throw() { if (ptr != NULL) free_file_line(ptr, NULL, 0, false); }
void operator delete[](void* ptr, const std::nothrow_t&) throw() { if (ptr != NULL) free_file_line(ptr, NULL, 0, true); }

#endif

//**************************************************************************
//  OPERATOR OVERLOADS - DEFINITIONS
//**************************************************************************

// file/line new/delete operators
void *operator new(std::size_t size, const char *file, int line) throw (std::bad_alloc) { return malloc_file_line(size, file, line, false, true, false); }
void *operator new[](std::size_t size, const char *file, int line) throw (std::bad_alloc) { return malloc_file_line(size, file, line, true, true, false); }
void operator delete(void *ptr, const char *file, int line) { if (ptr != NULL) free_file_line(ptr, file, line, false); }
void operator delete[](void *ptr, const char *file, int line) { if (ptr != NULL) free_file_line(ptr, file, line, true); }

// file/line new/delete operators with zeroing
void *operator new(std::size_t size, const char *file, int line, const zeromem_t &) throw (std::bad_alloc) { return malloc_file_line(size, file, line, false, true, true); }
void *operator new[](std::size_t size, const char *file, int line, const zeromem_t &) throw (std::bad_alloc) { return malloc_file_line(size, file, line, true, true, true); }
void operator delete(void *ptr, const char *file, int line, const zeromem_t &) { if (ptr != NULL) free_file_line(ptr, file, line, false); }
void operator delete[](void *ptr, const char *file, int line, const zeromem_t &) { if (ptr != NULL) free_file_line(ptr, file, line, true); }



//**************************************************************************
//  GLOBAL HELPERS
//**************************************************************************

//-------------------------------------------------
//  malloc_file_line - allocate memory with file
//  and line number information
//-------------------------------------------------

void *malloc_file_line(size_t size, const char *file, int line, bool array, bool throw_on_fail, bool clear)
{
	// allocate the memory and fail if we can't
	void *result = array ? osd_malloc_array(size) : osd_malloc(size);
	if (result == NULL)
	{
		fprintf(stderr, "Failed to allocate %d bytes (%s:%d)\n", UINT32(size), file, line);
		osd_break_into_debugger("Failed to allocate RAM");
		if (throw_on_fail)
			throw std::bad_alloc();
		return NULL;
	}

	// zap the memory if requested
	if (clear)
		memset(result, 0, size);

	// add a new entry
	memory_entry::allocate(size, result, file, line, array);

	return result;
}


//-------------------------------------------------
//  free_file_line - free memory with file
//  and line number information
//-------------------------------------------------

void free_file_line(void *memory, const char *file, int line, bool array)
{
	// find the memory entry
	memory_entry *entry = memory_entry::find(memory);

	// warn about untracked frees
	if (entry == NULL)
	{
		fprintf(stderr, "Error: attempt to free untracked memory %p in %s(%d)!\n", memory, file, line);
		osd_break_into_debugger("Error: attempt to free untracked memory");
		return;
	}

	// warn about mismatched arrays
	if (!array && entry->m_array)
	{
		fprintf(stderr, "Warning: attempt to free array %p with global_free in %s(%d)!\n", memory, file, line);
	}

	// free the entry and the memory
	memory_entry::release(entry, file, line);
	osd_free(memory);
}


//-------------------------------------------------
//  track_memory - enables or disables the memory
//  tracking
//-------------------------------------------------

void track_memory(bool track)
{
	memory_entry::s_tracking = track;
}


//-------------------------------------------------
//  next_memory_id - return the ID of the next
//  allocated block
//-------------------------------------------------

UINT64 next_memory_id()
{
	return memory_entry::s_curid;
}


//-------------------------------------------------
//  dump_unfreed_mem - called from the exit path
//  of any code that wants to check for unfreed
//  memory
//-------------------------------------------------

void dump_unfreed_mem(UINT64 start)
{
	memory_entry::report_unfreed(start);
}



//**************************************************************************
//  MEMORY ENTRY
//**************************************************************************

//-------------------------------------------------
//  acquire_lock - acquire the memory entry lock,
//  creating a new one if needed
//-------------------------------------------------

void memory_entry::acquire_lock()
{
	// allocate a lock on first usage
	// note that osd_lock_alloc() may re-enter this path, so protect against recursion!
	if (s_lock == NULL)
	{
		if (s_lock_alloc)
			return;
		s_lock_alloc = true;
		s_lock = osd_lock_alloc();
		s_lock_alloc = false;
	}
	osd_lock_acquire(s_lock);
}

//-------------------------------------------------
//  allocate - allocate a new memory entry
//-------------------------------------------------

memory_entry *memory_entry::allocate(size_t size, void *base, const char *file, int line, bool array)
{
	acquire_lock();

	// if we're out of free entries, allocate a new chunk
	if (s_freehead == NULL)
	{
		// create a new chunk, and fail if we can't
		memory_entry *entry = reinterpret_cast<memory_entry *>(osd_malloc_array(memory_block_alloc_chunk * sizeof(memory_entry)));
		if (entry == NULL)
		{
         osd_lock_release(s_lock);
			return NULL;
		}

		// add all the entries to the list
		for (int entrynum = 0; entrynum < memory_block_alloc_chunk; entrynum++)
		{
			entry->m_next = s_freehead;
			s_freehead = entry++;
		}
	}

	// grab a free entry
	memory_entry *entry = s_freehead;
	s_freehead = entry->m_next;

	// populate it
	entry->m_size = size;
	entry->m_base = base;
	entry->m_file = s_tracking ? file : NULL;
	entry->m_line = s_tracking ? line : 0;
	entry->m_id = s_curid++;
	entry->m_array = array;

	// add it to the alloc list
	int hashval = reinterpret_cast<FPTR>(base) % k_hash_prime;
	entry->m_next = s_hash[hashval];
	if (entry->m_next != NULL)
		entry->m_next->m_prev = entry;
	entry->m_prev = NULL;
	s_hash[hashval] = entry;

   osd_lock_release(s_lock);
	return entry;
}


//-------------------------------------------------
//  find - find a memory entry
//-------------------------------------------------

memory_entry *memory_entry::find(void *ptr)
{
	// NULL maps to nothing
	if (ptr == NULL)
		return NULL;

	// scan the list under the lock
	acquire_lock();

	int hashval = reinterpret_cast<FPTR>(ptr) % k_hash_prime;
	memory_entry *entry;
	for (entry = s_hash[hashval]; entry != NULL; entry = entry->m_next)
		if (entry->m_base == ptr)
			break;

   osd_lock_release(s_lock);
	return entry;
}


//-------------------------------------------------
//  release - release a memory entry
//-------------------------------------------------

void memory_entry::release(memory_entry *entry, const char *file, int line)
{
	acquire_lock();

	// remove ourselves from the alloc list
	int hashval = reinterpret_cast<FPTR>(entry->m_base) % k_hash_prime;
	if (entry->m_prev != NULL)
		entry->m_prev->m_next = entry->m_next;
	else
		s_hash[hashval] = entry->m_next;
	if (entry->m_next != NULL)
		entry->m_next->m_prev = entry->m_prev;

	// add ourself to the free list
	entry->m_next = s_freehead;
	s_freehead = entry;

   osd_lock_release(s_lock);
}

/**
 * @fn	void memory_entry::report_unfreed(UINT64 start)
 *
 * @brief	-------------------------------------------------
 * 			  report_unfreed - print a list of unfreed memory to the target file
 * 			-------------------------------------------------.
 *
 * @param	start	The start.
 */

void memory_entry::report_unfreed(UINT64 start)
{
	acquire_lock();

	// check for leaked memory
	UINT32 total = 0;

	for (int hashnum = 0; hashnum < k_hash_prime; hashnum++)
		for (memory_entry *entry = s_hash[hashnum]; entry != NULL; entry = entry->m_next)
			if (entry->m_file != NULL && entry->m_id >= start)
			{
				if (total == 0)
					fprintf(stderr, "--- memory leak warning ---\n");
				total += entry->m_size;
				fprintf(stderr, "#%06d, nofree %d bytes (%s:%d)\n", (UINT32)entry->m_id, static_cast<UINT32>(entry->m_size), entry->m_file, (int)entry->m_line);
			}

   osd_lock_release(s_lock);

	if (total > 0)
		fprintf(stderr, "a total of %u bytes were not freed\n", total);
}
