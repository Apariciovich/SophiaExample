#ifndef SI_READ_H_
#define SI_READ_H_

/*
 * sophia database
 * sphia.org
 *
 * Copyright (c) Dmitry Simonenko
 * BSD License
*/

typedef struct siread siread;

struct siread {
	ssorder   order;
	char     *key;
	char     *upsert;
	int       upsert_eq;
	char     *prefix;
	uint32_t  prefix_size;
	int       has;
	uint64_t  vlsn;
	svmerge   merge;
	int       read_start;
	int       read_disk;
	int       read_cache;
	svv      *result;
	sicache  *cache;
	sr       *r;
	si       *index;
};

int  si_readopen(siread*, si*, sicache*, ssorder,
                 uint64_t,
                 char*, char*,
                 char*, uint32_t, int, int);
int  si_readclose(siread*);
int  si_read(siread*);
int  si_readcommited(si*, sr*, svv*);

#endif
