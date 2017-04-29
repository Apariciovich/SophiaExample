#ifndef SC_H_
#define SC_H_

/*
 * sophia database
 * sphia.org
 *
 * Copyright (c) Dmitry Simonenko
 * BSD License
*/

typedef struct scdb scdb;
typedef struct sctask sctask;
typedef struct sc sc;

enum {
	SC_QGC     = 1,
	SC_QEXPIRE = 2,
	SC_QBACKUP = 3,
	SC_QMAX
};

struct scdb {
	uint32_t  workers[SC_QMAX];
	si       *index;
	/* state */
	uint32_t  expire;
	uint64_t  expire_time;
	uint64_t  gc_time;
	uint32_t  gc;
	uint32_t  backup;
};

struct sctask {
	scdb     *db;
	scworker *w;
	uint64_t  vlsn;
	uint64_t  time;
	int       rotate;
	int       gc;
	int       backup;
	siplan    plan;
};

struct sc {
	ssmutex       lock;
	uint32_t      prio[SC_QMAX];
	/* backup state */
	uint32_t      backup_bsn;
	uint32_t      backup_bsn_last;
	uint32_t      backup_bsn_last_complete;
	uint32_t      backup;
	uint32_t      backup_in_progress;
	char         *backup_path;
	/* index */
	int           rotate;
	int           rr;
	int           count;
	scdb         *i;
	/* pools */
	ssthreadpool  tp;
	scworkerpool  wp;
	swmanager    *wm;
	sr           *r;
};

int sc_init(sc*, sr*, swmanager*);
int sc_set(sc*, uint32_t);
int sc_setbackup(sc*, char*);
int sc_run(sc*, ssthreadf, void*, int);
int sc_shutdown(sc*);

static inline void
sc_register(sc *s, si *index)
{
	int pos = index->scheme.id;
	assert(pos < s->count);
	s->i[pos].index = index;
}

static inline scdb*
sc_of(sc *s, si *index)
{
	int pos = index->scheme.id;
	assert(pos < s->count);
	return &s->i[pos];
}

#endif
