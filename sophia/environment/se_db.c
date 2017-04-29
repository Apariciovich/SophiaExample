
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

static int
se_dbscheme_init(sedb *db, char *name, int size)
{
	se *e = se_of(&db->o);
	/* database id */
	uint32_t id = sr_seq(&e->seq, SR_DSN);
	sr_seq(&e->seq, SR_DSNNEXT);
	/* prepare index scheme */
	sischeme *scheme = db->scheme;
	if (size == 0)
		size = strlen(name);
	scheme->name = ss_malloc(&e->a, size + 1);
	if (ssunlikely(scheme->name == NULL))
		goto error;
	memcpy(scheme->name, name, size);
	scheme->name[size] = 0;
	scheme->id                    = id;
	scheme->sync                  = 1;
	scheme->mmap                  = 1;
	scheme->direct_io             = 0;
	scheme->direct_io_page_size   = 4096;
	scheme->direct_io_buffer_size = 8 * 1024 * 1024;
	scheme->compression           = 0;
	scheme->compression_if        = &ss_nonefilter;
	scheme->expire                = 0;
	scheme->buf_gc_wm             = 1024 * 1024;
	scheme->compression_sz =
		ss_strdup(&e->a, scheme->compression_if->name);
	if (ssunlikely(scheme->compression_sz == NULL))
		goto error;
	sf_upsertinit(&scheme->upsert);
	sf_schemeinit(&scheme->scheme);
	return 0;
error:
	sr_oom(&e->error);
	return -1;
}

static int
se_dbscheme_set(sedb *db)
{
	se *e = se_of(&db->o);
	sischeme *s = si_scheme(db->index);
	/* set default scheme */
	int rc;
	if (s->scheme.fields_count == 0)
	{
		sffield *field = sf_fieldnew(&e->a, "key");
		if (ssunlikely(field == NULL))
			return sr_oom(&e->error);
		rc = sf_fieldoptions(field, &e->a, "string,key(0)");
		if (ssunlikely(rc == -1)) {
			sf_fieldfree(field, &e->a);
			return sr_oom(&e->error);
		}
		rc = sf_schemeadd(&s->scheme, &e->a, field);
		if (ssunlikely(rc == -1)) {
			sf_fieldfree(field, &e->a);
			return sr_oom(&e->error);
		}
		field = sf_fieldnew(&e->a, "value");
		if (ssunlikely(field == NULL))
			return sr_oom(&e->error);
		rc = sf_fieldoptions(field, &e->a, "string");
		if (ssunlikely(rc == -1)) {
			sf_fieldfree(field, &e->a);
			return sr_oom(&e->error);
		}
		rc = sf_schemeadd(&s->scheme, &e->a, field);
		if (ssunlikely(rc == -1)) {
			sf_fieldfree(field, &e->a);
			return sr_oom(&e->error);
		}
	}
	/* validate scheme and set keys */
	rc = sf_schemevalidate(&s->scheme, &e->a);
	if (ssunlikely(rc == -1)) {
		sr_error(&e->error, "incomplete scheme", s->name);
		return -1;
	}
	/* validate io settings */
	if (s->mmap && s->direct_io) {
		sr_error(&e->error, "%s", "incompatible options: mmap and direct_io");
		return -1;
	}
	/* compression */
	s->compression_if = ss_filterof(s->compression_sz);
	if (ssunlikely(s->compression_if == NULL)) {
		sr_error(&e->error, "unknown compression type '%s'",
		         s->compression_sz);
		return -1;
	}
	s->compression = s->compression_if != &ss_nonefilter;
	/* path */
	if (s->path == NULL) {
		char path[1024];
		snprintf(path, sizeof(path), "%s/%s", e->rep_conf->path, s->name);
		s->path = ss_strdup(&e->a, path);
		if (ssunlikely(s->path == NULL))
			return sr_oom(&e->error);
	}
	/* backup path */
	s->path_backup = e->rep_conf->path_backup;
	if (e->rep_conf->path_backup) {
		s->path_backup = ss_strdup(&e->a, e->rep_conf->path_backup);
		if (ssunlikely(s->path_backup == NULL))
			return sr_oom(&e->error);
	}
	/* compaction settings */
	sicompaction *c = &s->compaction;
	/* convert periodic times from sec to usec */
	c->gc_period_us     = c->gc_period * 1000000;
	c->expire_period_us = c->expire_period * 1000000;

	/* .. */
	db->r->scheme = &s->scheme;
	db->r->upsert = &s->upsert;
	db->r->stat   = &db->stat;
	db->r->av     = &db->a;
	db->r->ptr    = db->index;
	return 0;
}

int se_dbopen(so *o)
{
	sedb *db = se_cast(o, sedb*, SEDB);
	se *e = se_of(&db->o);
	assert(sr_status(&e->status) == SR_RECOVER);
	int rc;
	rc = se_dbscheme_set(db);
	if (ssunlikely(rc == -1))
		return -1;
	sx_indexset(&db->coindex, db->scheme->id);
	sr_log(&e->log, "loading database '%s'", db->scheme->path);
	rc = si_open(db->index);
	if (ssunlikely(rc == -1)) {
		sr_statusset(&e->status, SR_MALFUNCTION);
		return -1;
	}
	db->created = rc;
	sc_register(&e->scheduler, db->index);
	return 0;
}

static inline int
se_dbfree(sedb *db)
{
	se *e = se_of(&db->o);
	int rcret = 0;
	int rc;
	rc = si_close(db->index);
	if (ssunlikely(rc == -1))
		rcret = -1;
	sf_limitfree(&db->limit, &e->a);
	sr_statfree(&db->stat);
	sx_indexfree(&db->coindex, &e->xm);
	ss_aclose(&db->a);
	so_mark_destroyed(&db->o);
	ss_free(&e->a, db);
	return rcret;
}

int se_dbdestroy(so *o)
{
	sedb *db = se_cast(o, sedb*, SEDB);
	se *e = se_of(&db->o);
	int status = sr_status(&e->status);
	if (status != SR_SHUTDOWN &&
	    status != SR_OFFLINE)
		return 0;
	return se_dbfree(db);
}

static inline int
se_dbwrite(sedb *db, sedocument *o, uint8_t flags)
{
	se *e = se_of(&db->o);
	if (ssunlikely(! se_active(e)))
		goto error;

	/* create document */
	int rc;
	rc = se_document_validate(o, &db->o);
	if (ssunlikely(rc == -1))
		goto error;
	rc = se_document_create(o, flags);
	if (ssunlikely(rc == -1))
		goto error;

	svv *v = o->v;
	sv_vref(v);
	so_destroy(&o->o);

	/* single-statement transaction */
	svlog log;
	rc = sv_loginit(&log, db->r, e->db.n);
	if (ssunlikely(rc == -1))
		return -1;
	sv_loginit_index(&log, db->index->scheme.id, db->r);

	sx x;
	sxstate state =
		sx_set_autocommit(&e->xm, &db->coindex, &x, &log, v);
	if (ssunlikely(state != SX_COMMIT)) {
		/* rollback */
		sv_logfree(&log, db->r);
		return 1;
	}

	/* write wal and index */
	rc = sc_commit(&e->scheduler, &log, 0, 0);
	if (ssunlikely(rc == -1)) {
		svlogv *lv = sv_logat(&log, 0);
		sv_vunref(db->r, lv->v);
	}
	sv_logfree(&log, db->r);

	sx_gc(&x);
	return rc;

error:
	so_destroy(&o->o);
	return -1;
}

static int
se_dbset(so *o, so *v)
{
	sedb *db = se_cast(o, sedb*, SEDB);
	sedocument *key = se_cast(v, sedocument*, SEDOCUMENT);
	uint64_t start = ss_utime();
	int rc = se_dbwrite(db, key, 0);
	sr_statset(&db->stat, start);
	return rc;
}

static int
se_dbupsert(so *o, so *v)
{
	sedb *db = se_cast(o, sedb*, SEDB);
	sedocument *key = se_cast(v, sedocument*, SEDOCUMENT);
	se *e = se_of(&db->o);
	uint64_t start = ss_utime();
	if (! sf_upserthas(&db->scheme->upsert)) {
		if (key->created <= 1)
			so_destroy(v);
		sr_error(&e->error, "%s", "upsert callback is not set");
		return -1;
	}
	int rc = se_dbwrite(db, key, SVUPSERT);
	sr_statupsert(&db->stat, start);
	return rc;
}

static int
se_dbdel(so *o, so *v)
{
	sedb *db = se_cast(o, sedb*, SEDB);
	sedocument *key = se_cast(v, sedocument*, SEDOCUMENT);
	uint64_t start = ss_utime();
	int rc = se_dbwrite(db, key, SVDELETE);
	sr_statdelete(&db->stat, start);
	return rc;
}

static void*
se_dbget(so *o, so *v)
{
	sedb *db = se_cast(o, sedb*, SEDB);
	sedocument *key = se_cast(v, sedocument*, SEDOCUMENT);
	uint64_t vlsn = sr_seq(db->r->seq, SR_LSN);
	return se_read(db, key, NULL, vlsn, NULL);
}

static void*
se_dbdocument(so *o)
{
	sedb *db = se_cast(o, sedb*, SEDB);
	se *e = se_of(&db->o);
	return se_document_new(e, &db->o, NULL);
}

static soif sedbif =
{
	.open         = NULL,
	.destroy      = NULL,
	.free         = NULL,
	.document     = se_dbdocument,
	.setstring    = NULL,
	.setint       = NULL,
	.getobject    = NULL,
	.getstring    = NULL,
	.getint       = NULL,
	.set          = se_dbset,
	.upsert       = se_dbupsert,
	.del          = se_dbdel,
	.get          = se_dbget,
	.begin        = NULL,
	.prepare      = NULL,
	.commit       = NULL,
	.cursor       = NULL,
};

so *se_dbnew(se *e, char *name, int size)
{
	sedb *o = ss_malloc(&e->a, sizeof(sedb));
	if (ssunlikely(o == NULL)) {
		sr_oom(&e->error);
		return NULL;
	}
	memset(o, 0, sizeof(*o));
	so_init(&o->o, &se_o[SEDB], &sedbif, &e->o, &e->o);
	sr_statinit(&o->stat);
	int rc;
	rc = sf_limitinit(&o->limit, &e->a);
	if (ssunlikely(rc == -1)) {
		ss_free(&e->a, o);
		return NULL;
	}
	o->a = e->a;
	o->index = si_init(&e->r, &o->o);
	if (ssunlikely(o->index == NULL)) {
		sf_limitfree(&o->limit, &e->a);
		ss_free(&e->a, o);
		return NULL;
	}
	o->r = si_r(o->index);
	o->scheme = si_scheme(o->index);

	rc = se_dbscheme_init(o, name, size);
	if (ssunlikely(rc == -1)) {
		sf_limitfree(&o->limit, &e->a);
		si_close(o->index);
		ss_free(&e->a, o);
		return NULL;
	}
	sx_indexinit(&o->coindex, &e->xm, o->r, &o->o);
	return &o->o;
}

so *se_dbmatch(se *e, char *name)
{
	sslist *i;
	ss_listforeach(&e->db.list, i) {
		sedb *db = (sedb*)sscast(i, so, link);
		if (strcmp(db->scheme->name, name) == 0)
			return &db->o;
	}
	return NULL;
}

so *se_dbmatch_id(se *e, uint32_t id)
{
	sslist *i;
	ss_listforeach(&e->db.list, i) {
		sedb *db = (sedb*)sscast(i, so, link);
		if (db->scheme->id == id)
			return &db->o;
	}
	return NULL;
}
