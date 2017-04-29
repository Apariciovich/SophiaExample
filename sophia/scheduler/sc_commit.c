
/*
 * sophia database
 * sphia.org
 *
 * Copyright (c) Dmitry Simonenko
 * BSD License
*/

#include <libss.h>
#include <libsf.h>
#include <libsr.h>
#include <libso.h>
#include <libsv.h>
#include <libsd.h>
#include <libsw.h>
#include <libsi.h>
#include <libsy.h>
#include <libsc.h>

int sc_commit(sc *s, svlog *log, uint64_t lsn, int recover)
{
	/* write-ahead log */
	swtx tl;
	sw_begin(s->wm, &tl, lsn, recover);
	int rc = sw_write(&tl, log);
	if (ssunlikely(rc == -1)) {
		sw_rollback(&tl);
		return -1;
	}
	sw_commit(&tl);

	/* index */
	svlogindex *i   = (svlogindex*)log->index.s;
	svlogindex *end = (svlogindex*)log->index.p;
	for (; i < end; i++) {
		if (i->count == 0)
			continue;
		si *index = i->r->ptr;
		sitx x;
		si_begin(&x, index);
		si_write(&x, log, i, recover);
		si_commit(&x);
	}
	return 0;
}
