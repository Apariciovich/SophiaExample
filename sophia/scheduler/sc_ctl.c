
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

int sc_ctl_call(sc *s, uint64_t vlsn)
{
	int rc = sr_statusactive(s->r->status);
	if (ssunlikely(rc == 0))
		return 0;
	scworker *w = sc_workerpool_pop(&s->wp, s->r);
	if (ssunlikely(w == NULL))
		return -1;
	rc = sc_step(s, w, vlsn);
	sc_workerpool_push(&s->wp, w);
	return rc;
}

int sc_ctl_compaction(sc *s, uint64_t vlsn, si *index)
{
	sr *r = s->r;
	int rc = sr_statusactive(r->status);
	if (ssunlikely(rc == 0))
		return 0;
	scworker *w = sc_workerpool_pop(&s->wp, r);
	if (ssunlikely(w == NULL))
		return -1;
	siplan plan = {
		.plan = SI_COMPACTION,
		.node = NULL
	};
	rc = si_plan(index, &plan);
	if (rc)
		rc = si_execute(index, &w->dc, &plan, vlsn);
	sc_workerpool_push(&s->wp, w);
	return rc;
}

int sc_ctl_expire(sc *s, si *index)
{
	ss_mutexlock(&s->lock);
	scdb *db = sc_of(s, index);
	sc_task_expire(db);
	ss_mutexunlock(&s->lock);
	return 0;
}

int sc_ctl_gc(sc *s, si *index)
{
	ss_mutexlock(&s->lock);
	scdb *db = sc_of(s, index);
	sc_task_gc(db);
	ss_mutexunlock(&s->lock);
	return 0;
}

int sc_ctl_backup(sc *s)
{
	int rc = sc_backupstart(s);
	if (ssunlikely(rc == -1))
		return -1;
	if (ssunlikely(rc == 1))
		return 0;
	rc = sc_backupbegin(s);
	if (ssunlikely(rc == -1))
		sc_backupstop(s);
	return rc;
}
