/*
 *	aprsc
 *
 *	(c) Matti Aarnio, OH2MQK, <oh2mqk@sral.fi>
 *
 *    This program is free software; you can redistribute it and/or modify
 *    it under the terms of the GNU General Public License as published by
 *    the Free Software Foundation; either version 2 of the License, or
 *    (at your option) any later version.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU General Public License for more details.
 *
 *    You should have received a copy of the GNU General Public License
 *    along with this program; if not, write to the Free Software
 *    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *	
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <pthread.h>
#include <sys/mman.h>

#include "cellmalloc.h"
#include "hmalloc.h"
#include "hlog.h"

/*
 *   cellmalloc() -- manages arrays of cells of data 
 *
 */

#ifndef _FOR_VALGRIND_
struct cellhead;

struct cellarena_t {
	int	cellsize;
	int	alignment;
	int	increment; /* alignment overhead applied.. */
	int	lifo_policy;
  	int	minfree;
	int	use_mutex;

	pthread_mutex_t mutex;

  	struct cellhead *free_head;
  	struct cellhead *free_tail;

	int	 freecount;
	int	 createsize;

	int	 cellblocks_count;
#define CELLBLOCKS_MAX 1000
	char	*cellblocks[CELLBLOCKS_MAX];	/* ref as 'char pointer' for pointer arithmetics... */
};

#define CELLHEAD_DEBUG 0

struct cellhead {
#if CELLHEAD_DEBUG == 1
	struct cellarena_t *ca;
#endif
	struct cellhead *next;
};


/*
 * new_cellblock() -- must be called MUTEX PROTECTED
 *
 */

int new_cellblock(cellarena_t *ca)
{
	int i;
	char *cb;

	cb = mmap( NULL, ca->createsize, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
	if (cb == NULL || cb == (char*)-1)
	  return -1;

	if (ca->cellblocks_count >= CELLBLOCKS_MAX) return -1;

	ca->cellblocks[ca->cellblocks_count++] = cb;

	for (i = 0; i <= ca->createsize-ca->increment; i += ca->increment) {
		struct cellhead *ch = (struct cellhead *)(cb + i); /* pointer arithmentic! */
		if (!ca->free_head) {
		  ca->free_head = ch;
		} else {
		  ca->free_tail->next = ch;
		}
		ca->free_tail = ch;
		ch->next = NULL;
#if CELLHEAD_DEBUG == 1
		ch->ca   = ca; // cellhead pointer space
#endif

		ca->freecount += 1;
	}

	hlog( LOG_DEBUG, "new_cellblock(%p) of %dB freecount %d  returns to %p/%p",
	      ca, ca->cellsize, ca->freecount,
	      __builtin_return_address(1), __builtin_return_address(2) );

	return 0;
}



/*
 * cellinit()  -- the main program calls this once for each used cell type/size
 *
 */


cellarena_t *cellinit(int cellsize, int alignment, int policy, int createkb, int minfree)
{
	cellarena_t *ca = hmalloc(sizeof(*ca));
	memset(ca, 0, sizeof(*ca));

#if CELLHEAD_DEBUG == 1
	if (alignment < __alignof__(void*))
		alignment = __alignof__(void*);   // cellhead pointer space
#endif

	ca->cellsize  = cellsize;
	ca->alignment = alignment;
	ca->minfree   = minfree;
#if CELLHEAD_DEBUG == 1
	ca->increment = cellsize + sizeof(void*); // cellhead pointer space
#else
	ca->increment = cellsize;
#endif
	if ((cellsize % alignment) != 0) {
		ca->increment +=  alignment - cellsize % alignment;
	}
	ca->lifo_policy =  policy & CELLMALLOC_POLICY_LIFO;
	ca->use_mutex   = (policy & CELLMALLOC_POLICY_NOMUTEX) ? 0 : 1;

	ca->createsize = createkb * 1024;


	pthread_mutex_init(&ca->mutex, NULL);

	new_cellblock(ca); /* First block of cells, not yet need to be mutex protected */
	while (ca->freecount < ca->minfree)
		new_cellblock(ca); /* more until minfree is full */

#if CELLHEAD_DEBUG == 1
	hlog(LOG_DEBUG, "cellinit()  cellhead=%p", ca);
#endif
	return ca;
}


inline void *cellhead_to_clientptr(struct cellhead *ch)
{
	char *p = (char*)ch;
#if CELLHEAD_DEBUG == 1
	p += sizeof(void*);
#endif
	return p;
}

inline struct cellhead *clientptr_to_cellhead(void *v)
{
#if CELLHEAD_DEBUG == 1
	struct cellhead *ch = (struct cellhead *)(((char*)v) - sizeof(void*));
#else
	struct cellhead *ch = (struct cellhead*)v;
#endif
	return ch;
}


void *cellmalloc(cellarena_t *ca)
{
	void *cp;
	struct cellhead *ch;

	if (ca->use_mutex)
		pthread_mutex_lock(&ca->mutex);

	while (!ca->free_head  || (ca->freecount < ca->minfree))
		if (new_cellblock(ca)) {
			pthread_mutex_unlock(&ca->mutex);
			return NULL;
		}

	/* Pick new one off the free-head ! */
	ch = ca->free_head;
	ca->free_head = ch->next;
	ch->next = NULL;
	cp = ch;
	if (ca->free_head == NULL)
	  ca->free_tail = NULL;

	ca->freecount -= 1;

	if (ca->use_mutex)
		pthread_mutex_unlock(&ca->mutex);

	// hlog(LOG_DEBUG, "cellmalloc(%p at %p) freecount %d", cellhead_to_clientptr(cp), ca, ca->freecount);

	return cellhead_to_clientptr(cp);
}

/*
 *  cellmallocmany() -- give many cells in single lock region
 *
 */

int   cellmallocmany(cellarena_t *ca, void **array, int numcells)
{
	int count;
	struct cellhead *ch;

	if (ca->use_mutex)
		pthread_mutex_lock(&ca->mutex);

	for (count = 0; count < numcells; ++count) {

		while (!ca->free_head ||
		       ca->freecount < ca->minfree) {
			/* Out of free cells ? alloc new set */
			if (new_cellblock(ca)) {
			  /* Failed ! */
			  break;
			}
		}

		/* Pick new one off the free-head ! */

		ch = ca->free_head;

		// hlog( LOG_DEBUG, "cellmallocmany(%d of %d); freecount %d; %p at %p",
		//       count, numcells, ca->freecount, cellhead_to_clientptr(ch), ca );

		// if (!ch)
		// 	break;	// Should not happen...

		ca->free_head = ch->next;
		ch->next = NULL;

		if (ca->free_head == NULL)
			ca->free_tail = NULL;

		array[count] = cellhead_to_clientptr(ch);

		ca->freecount -= 1;

	}

	if (ca->use_mutex)
		pthread_mutex_unlock(&ca->mutex);

	return count;
}



void  cellfree(cellarena_t *ca, void *p)
{
	struct cellhead *ch = clientptr_to_cellhead(p);
	ch->next = NULL;
#if CELLHEAD_DEBUG == 1
	if (ch->ca != ca) {
	  hlog(LOG_ERR, "cellfree(%p to %p) wrong cellhead->ca pointer %p", p, ca, ch->ca);
	}
#endif

	// hlog(LOG_DEBUG, "cellfree() %p to %p", p, ca);

	if (ca->use_mutex)
		pthread_mutex_lock(&ca->mutex);

	if (ca->lifo_policy) {
	  /* Put the cell on free-head */
	  ch->next = ca->free_head;
	  ca->free_head = ch;

	} else {
	  /* Put the cell on free-tail */
	  if (ca->free_tail)
	    ca->free_tail->next = ch;
	  ca->free_tail = ch;
	  if (!ca->free_head)
	    ca->free_head = ch;
	  ch->next = NULL;
	}

	ca->freecount += 1;

	if (ca->use_mutex)
		pthread_mutex_unlock(&ca->mutex);
}

/*
 *  cellfreemany() -- release many cells in single lock region
 *
 */

void  cellfreemany(cellarena_t *ca, void **array, int numcells)
{
	int count;

	if (ca->use_mutex)
		pthread_mutex_lock(&ca->mutex);

	for (count = 0; count < numcells; ++count) {

	  struct cellhead *ch = clientptr_to_cellhead(array[count]);

#if CELLHEAD_DEBUG == 1
	  if (ch->ca != ca) {
	    hlog(LOG_ERR, "cellfreemany(%p to %p) wrong cellhead->ca pointer %p", array[count], ca, ch->ca);
	  }
#endif

	  // hlog(LOG_DEBUG, "cellfreemany() %p to %p", ch, ca);

	  if (ca->lifo_policy) {
	    /* Put the cell on free-head */
	    ch->next = ca->free_head;
	    ca->free_head = ch;

	  } else {
	    /* Put the cell on free-tail */
	    if (ca->free_tail)
	      ca->free_tail->next = ch;
	    ca->free_tail = ch;
	    if (!ca->free_head)
	      ca->free_head = ch;
	    ch->next = NULL;
	  }

	  ca->freecount += 1;

	}

	if (ca->use_mutex)
		pthread_mutex_unlock(&ca->mutex);
}
#endif /* (NOT) _FOR_VALGRIND_ */
