
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
#include <libsw.h>
#include <libsd.h>
#include <libsi.h>
#include <libsx.h>
#include <libsy.h>
#include <libsc.h>
#include <libse.h>

static inline so*
se_readresult(se *e, sedb *db, siread *r)
{
	sedocument *v =
		(sedocument*)se_document_new(e, r->index->object, r->result);
	if (ssunlikely(v == NULL))
		return NULL;
	v->read_disk    = r->read_disk;
	v->read_cache   = r->read_cache;
	v->read_latency = 0;
	if (r->result) {
		v->read_latency = ss_utime() - r->read_start;
		sr_statget(&db->stat,
		           v->read_latency,
		           v->read_disk,
		           v->read_cache);
	}

	/* propagate current document settings to
	 * the result one */
	v->orderset = 1;
	v->order = r->order;
	if (v->order == SS_GTE)
		v->order = SS_GT;
	else
	if (v->order == SS_LTE)
		v->order = SS_LT;

	/* set prefix */
	if (r->prefix) {
		v->prefix = r->prefix;
		v->prefix_copy = r->prefix;
		v->prefix_size = r->prefix_size;
	}

	v->created   = 1;
	return &v->o;
}

so *se_read(sedb *db, sedocument *o, sx *x, uint64_t vlsn,
            sicache *cache)
{
	se *e = se_of(&db->o);
	if (ssunlikely(! se_active(e)))
		goto error;

	uint64_t start = ss_utime();

	/* prepare the key */
	int rc = se_document_validate_ro(o, &db->o);
	if (ssunlikely(rc == -1))
		goto error;
	rc = se_document_createkey(o);
	if (ssunlikely(rc == -1))
		goto error;

	sedocument *ret = NULL;
	svv *vup = NULL;

	/* concurrent */
	if (x && o->order == SS_EQ) {
		/* note: prefix is ignored during concurrent
		 * index search */
		int rc = sx_get(x, &db->coindex, o->v, &vup);
		if (ssunlikely(rc == -1 || rc == 2 /* delete */))
			goto error;
		if (rc == 1 && !sf_is(db->r->scheme, sv_vpointer(vup), SVUPSERT))
		{
			ret = (sedocument*)se_document_new(e, &db->o, vup);
			if (sslikely(ret)) {
				ret->created  = 1;
				ret->orderset = 1;
			} else {
				sv_vunref(db->r, vup);
			}
			so_destroy(&o->o);
			return &ret->o;
		}
	} else {
		sx_get_autocommit(&e->xm, &db->coindex);
	}

	/* prepare read cache */
	int cachegc = 0;
	if (cache == NULL) {
		cachegc = 1;
		cache = si_cachepool_pop(&e->cachepool);
		if (ssunlikely(cache == NULL)) {
			if (vup)
				sv_vunref(db->r, vup);
			sr_oom(&e->error);
			goto error;
		}
	}

	sv_vref(o->v);

	/* do read */
	siread rq;
	si_readopen(&rq, db->index, cache, o->order,
	            vlsn,
	            sv_vpointer(o->v),
	            vup ? sv_vpointer(vup): NULL,
	            o->prefix_copy,
	            o->prefix_size,
	            0,
	            start);
	rc = si_read(&rq);
	si_readclose(&rq);

	/* prepare result */
	if (rc == 1) {
		ret = (sedocument*)se_readresult(e, db, &rq);
		if (ret)
			o->prefix_copy = NULL;
	}

	/* cleanup */
	if (o->v)
		sv_vunref(db->r, o->v);
	if (vup)
		sv_vunref(db->r, vup);
	if (ret == NULL && rq.result)
		sv_vunref(db->r, rq.result);
	if (cachegc && cache)
		si_cachepool_push(cache);

	so_destroy(&o->o);
	return &ret->o;
error:
	so_destroy(&o->o);
	return NULL;
}

