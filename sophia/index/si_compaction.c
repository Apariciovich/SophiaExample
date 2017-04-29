
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
#include <libsi.h>

static int
si_redistribute(si *index, sr *r, sdc *c, sinode *node, ssbuf *result)
{
	(void)index;
	int rc;
	svindex *vindex = si_nodeindex(node);
	ssiter i;
	ss_iterinit(sv_indexiter, &i);
	ss_iteropen(sv_indexiter, &i, r, vindex, SS_GTE, NULL);
	while (ss_iterhas(sv_indexiter, &i))
	{
		svv *v = sv_vv(ss_iterof(sv_indexiter, &i));
		rc = ss_bufadd(&c->b, r->a, &v, sizeof(svv**));
		if (ssunlikely(rc == -1))
			return sr_oom_malfunction(r->e);
		ss_iternext(sv_indexiter, &i);
	}
	if (ssunlikely(ss_bufused(&c->b) == 0))
		return 0;
	ss_iterinit(ss_bufiterref, &i);
	ss_iteropen(ss_bufiterref, &i, &c->b, sizeof(svv*));
	ssiter j;
	ss_iterinit(ss_bufiterref, &j);
	ss_iteropen(ss_bufiterref, &j, result, sizeof(sinode*));
	sinode *prev = ss_iterof(ss_bufiterref, &j);
	ss_iternext(ss_bufiterref, &j);
	while (1)
	{
		sinode *p = ss_iterof(ss_bufiterref, &j);
		if (p == NULL) {
			assert(prev != NULL);
			while (ss_iterhas(ss_bufiterref, &i)) {
				svv *v = ss_iterof(ss_bufiterref, &i);
				v->next = NULL;
				sv_indexset(&prev->i0, r, v);
				ss_iternext(ss_bufiterref, &i);
			}
			break;
		}
		while (ss_iterhas(ss_bufiterref, &i))
		{
			svv *v = ss_iterof(ss_bufiterref, &i);
			v->next = NULL;
			sdindexpage *page = sd_indexmin(&p->index);
			rc = sf_compare(r->scheme, sv_vpointer(v),
			                sd_indexpage_min(&p->index, page));
			if (ssunlikely(rc >= 0))
				break;
			sv_indexset(&prev->i0, r, v);
			ss_iternext(ss_bufiterref, &i);
		}
		if (ssunlikely(! ss_iterhas(ss_bufiterref, &i)))
			break;
		prev = p;
		ss_iternext(ss_bufiterref, &j);
	}
	assert(ss_iterof(ss_bufiterref, &i) == NULL);
	return 0;
}

static inline void
si_redistribute_set(si *index, sr *r, svv *v)
{
	/* match node */
	ssiter i;
	ss_iterinit(si_iter, &i);
	ss_iteropen(si_iter, &i, r, index, SS_GTE, sv_vpointer(v));
	sinode *node = ss_iterof(si_iter, &i);
	assert(node != NULL);
	/* update node */
	svindex *vindex = si_nodeindex(node);
	sv_indexset(vindex, r, v);
	node->used += sv_vsize(v, &index->r);
	/* schedule node */
	si_plannerupdate(&index->p, node);
}

static int
si_redistribute_index(si *index, sr *r, sdc *c, sinode *node)
{
	svindex *vindex = si_nodeindex(node);
	ssiter i;
	ss_iterinit(sv_indexiter, &i);
	ss_iteropen(sv_indexiter, &i, r, vindex, SS_GTE, NULL);
	while (ss_iterhas(sv_indexiter, &i)) {
		svv *v = sv_vv(ss_iterof(sv_indexiter, &i));
		int rc = ss_bufadd(&c->b, r->a, &v, sizeof(svv**));
		if (ssunlikely(rc == -1))
			return sr_oom_malfunction(r->e);
		ss_iternext(sv_indexiter, &i);
	}
	if (ssunlikely(ss_bufused(&c->b) == 0))
		return 0;
	ss_iterinit(ss_bufiterref, &i);
	ss_iteropen(ss_bufiterref, &i, &c->b, sizeof(svv*));
	while (ss_iterhas(ss_bufiterref, &i)) {
		svv *v = ss_iterof(ss_bufiterref, &i);
		v->next = NULL;
		si_redistribute_set(index, r, v);
		ss_iternext(ss_bufiterref, &i);
	}
	return 0;
}

static int
si_splitfree(ssbuf *result, sr *r)
{
	ssiter i;
	ss_iterinit(ss_bufiterref, &i);
	ss_iteropen(ss_bufiterref, &i, result, sizeof(sinode*));
	while (ss_iterhas(ss_bufiterref, &i))
	{
		sinode *p = ss_iterof(ss_bufiterref, &i);
		si_nodefree(p, r, 0);
		ss_iternext(ss_bufiterref, &i);
	}
	return 0;
}

static inline int
si_split(si *index, sdc *c, ssbuf *result,
         sinode   *parent,
         ssiter   *i,
         uint64_t  size_node,
         uint64_t  size_stream,
         uint32_t  stream,
         uint64_t  vlsn)
{
	sr *r = &index->r;
	uint32_t timestamp = ss_timestamp();
	int rc;
	sdmergeconf mergeconf = {
		.stream              = stream,
		.size_stream         = size_stream,
		.size_node           = size_node,
		.size_page           = index->scheme.compaction.node_page_size,
		.checksum            = index->scheme.compaction.node_page_checksum,
		.expire              = index->scheme.expire,
		.timestamp           = timestamp,
		.compression         = index->scheme.compression,
		.compression_if      = index->scheme.compression_if,
		.direct_io           = index->scheme.direct_io,
		.direct_io_page_size = index->scheme.direct_io_page_size,
		.vlsn                = vlsn
	};
	sinode *n = NULL;
	sdmerge merge;
	rc = sd_mergeinit(&merge, r, i, &c->build, &c->build_index,
	                  &c->upsert, &mergeconf);
	if (ssunlikely(rc == -1))
		return -1;
	while ((rc = sd_merge(&merge)) > 0)
	{
		/* create new node */
		uint64_t id = sr_seq(index->r.seq, SR_NSNNEXT);
		n = si_nodenew(r, id, parent->id);
		if (ssunlikely(n == NULL))
			goto error;
		rc = si_nodecreate(n, r, &index->scheme);
		if (ssunlikely(rc == -1))
			goto error;

		/* write pages */
		uint64_t offset;
		offset = sd_iosize(&c->io, &n->file);
		while ((rc = sd_mergepage(&merge, offset)) == 1) {
			rc = sd_writepage(r, &n->file, &c->io, merge.build);
			if (ssunlikely(rc == -1))
				goto error;
			offset = sd_iosize(&c->io, &n->file);
		}
		if (ssunlikely(rc == -1))
			goto error;

		offset = sd_iosize(&c->io, &n->file);
		rc = sd_mergeend(&merge, offset);
		if (ssunlikely(rc == -1))
			goto error;

		/* write index */
		rc = sd_writeindex(r, &n->file, &c->io, &merge.index);
		if (ssunlikely(rc == -1))
			goto error;

		/* mmap mode */
		if (index->scheme.mmap) {
			rc = si_nodemap(n, r);
			if (ssunlikely(rc == -1))
				goto error;
		}

		/* add node to the list */
		rc = ss_bufadd(result, index->r.a, &n, sizeof(sinode*));
		if (ssunlikely(rc == -1)) {
			sr_oom_malfunction(index->r.e);
			goto error;
		}

		n->index = merge.index;
	}
	if (ssunlikely(rc == -1))
		goto error;
	return 0;
error:
	if (n)
		si_nodefree(n, r, 0);
	sd_mergefree(&merge);
	si_splitfree(result, r);
	return -1;
}

static int
si_merge(si *index, sdc *c, sinode *node,
         uint64_t vlsn,
         ssiter *stream,
         uint64_t size_stream,
         uint32_t n_stream)
{
	sr *r = &index->r;
	ssbuf *result = &c->a;
	ssiter i;

	/* begin compaction.
	 *
	 * Split merge stream into a number of
	 * a new nodes.
	 */
	int rc;
	rc = si_split(index, c, result,
	              node, stream,
	              index->scheme.compaction.node_size,
	              size_stream,
	              n_stream,
	              vlsn);
	if (ssunlikely(rc == -1))
		return -1;

	SS_INJECTION(r->i, SS_INJECTION_SI_COMPACTION_0,
	             si_splitfree(result, r);
	             sr_malfunction(r->e, "%s", "error injection");
	             return -1);

	/* mask removal of a single node as a
	 * single node update */
	int count = ss_bufused(result) / sizeof(sinode*);
	int count_index;

	si_lock(index);
	count_index = index->n;
	si_unlock(index);

	sinode *n;
	if (ssunlikely(count == 0 && count_index == 1))
	{
		n = si_bootstrap(index, node->id);
		if (ssunlikely(n == NULL))
			return -1;
		rc = ss_bufadd(result, r->a, &n, sizeof(sinode*));
		if (ssunlikely(rc == -1)) {
			sr_oom_malfunction(r->e);
			si_nodefree(n, r, 1);
			return -1;
		}
		count++;
	}

	/* commit compaction changes */
	si_lock(index);
	svindex *j = si_nodeindex(node);
	si_plannerremove(&index->p, node);
	si_nodesplit(node);
	switch (count) {
	case 0: /* delete */
		si_remove(index, node);
		si_redistribute_index(index, r, c, node);
		break;
	case 1: /* self update */
		n = *(sinode**)result->s;
		n->i0 = *j;
		n->used = j->used;
		si_nodelock(n);
		si_replace(index, node, n);
		si_plannerupdate(&index->p, n);
		break;
	default: /* split */
		rc = si_redistribute(index, r, c, node, result);
		if (ssunlikely(rc == -1)) {
			si_unlock(index);
			si_splitfree(result, r);
			return -1;
		}
		ss_iterinit(ss_bufiterref, &i);
		ss_iteropen(ss_bufiterref, &i, result, sizeof(sinode*));
		n = ss_iterof(ss_bufiterref, &i);
		n->used = n->i0.used;
		si_nodelock(n);
		si_replace(index, node, n);
		si_plannerupdate(&index->p, n);
		for (ss_iternext(ss_bufiterref, &i); ss_iterhas(ss_bufiterref, &i);
		     ss_iternext(ss_bufiterref, &i)) {
			n = ss_iterof(ss_bufiterref, &i);
			n->used = n->i0.used;
			si_nodelock(n);
			si_insert(index, n);
			si_plannerupdate(&index->p, n);
		}
		break;
	}
	sv_indexinit(j);
	si_unlock(index);

	/* compaction completion */

	/* seal nodes */
	ss_iterinit(ss_bufiterref, &i);
	ss_iteropen(ss_bufiterref, &i, result, sizeof(sinode*));
	while (ss_iterhas(ss_bufiterref, &i))
	{
		n  = ss_iterof(ss_bufiterref, &i);
		if (index->scheme.sync) {
			rc = ss_filesync(&n->file);
			if (ssunlikely(rc == -1)) {
				sr_malfunction(r->e, "db file '%s' sync error: %s",
				               ss_pathof(&n->file.path),
				               strerror(errno));
				return -1;
			}
		}
		rc = si_noderename_seal(n, r, &index->scheme);
		if (ssunlikely(rc == -1)) {
			si_nodefree(node, r, 0);
			return -1;
		}
		SS_INJECTION(r->i, SS_INJECTION_SI_COMPACTION_3,
		             si_nodefree(node, r, 0);
		             sr_malfunction(r->e, "%s", "error injection");
		             return -1);
		ss_iternext(ss_bufiterref, &i);
	}

	SS_INJECTION(r->i, SS_INJECTION_SI_COMPACTION_1,
	             si_nodefree(node, r, 0);
	             sr_malfunction(r->e, "%s", "error injection");
	             return -1);

	/* gc node */
	uint16_t refs = si_noderefof(node);
	if (sslikely(refs == 0)) {
		rc = si_nodefree(node, r, 1);
		if (ssunlikely(rc == -1))
			return -1;
	} else {
		/* node concurrently being read, schedule for
		 * delayed removal */
		si_nodegc(node, r, &index->scheme);
		si_lock(index);
		ss_listappend(&index->gc, &node->gc);
		index->gc_count++;
		si_unlock(index);
	}

	SS_INJECTION(r->i, SS_INJECTION_SI_COMPACTION_2,
	             sr_malfunction(r->e, "%s", "error injection");
	             return -1);

	/* complete new nodes */
	ss_iterinit(ss_bufiterref, &i);
	ss_iteropen(ss_bufiterref, &i, result, sizeof(sinode*));
	while (ss_iterhas(ss_bufiterref, &i))
	{
		n = ss_iterof(ss_bufiterref, &i);
		rc = si_noderename_complete(n, r, &index->scheme);
		if (ssunlikely(rc == -1))
			return -1;
		SS_INJECTION(r->i, SS_INJECTION_SI_COMPACTION_4,
		             sr_malfunction(r->e, "%s", "error injection");
		             return -1);
		ss_iternext(ss_bufiterref, &i);
	}

	/* unlock */
	si_lock(index);
	ss_iterinit(ss_bufiterref, &i);
	ss_iteropen(ss_bufiterref, &i, result, sizeof(sinode*));
	while (ss_iterhas(ss_bufiterref, &i))
	{
		n = ss_iterof(ss_bufiterref, &i);
		si_nodeunlock(n);
		ss_iternext(ss_bufiterref, &i);
	}
	si_unlock(index);
	return 0;
}

int si_compaction(si *index, sdc *c, siplan *plan, uint64_t vlsn)
{
	sr *r = &index->r;
	sinode *node = plan->node;
	assert(node->flags & SI_LOCK);

	si_lock(index);
	svindex *vindex;
	vindex = si_noderotate(node);
	si_unlock(index);

	uint64_t size_stream = vindex->used;
	ssiter vindex_iter;
	ss_iterinit(sv_indexiter, &vindex_iter);
	ss_iteropen(sv_indexiter, &vindex_iter, &index->r, vindex, SS_GTE, NULL);

	/* prepare direct_io stream */
	int rc;
	if (index->scheme.direct_io) {
		rc = sd_ioprepare(&c->io, r,
		                  index->scheme.direct_io,
		                  index->scheme.direct_io_page_size,
		                  index->scheme.direct_io_buffer_size);
		if (ssunlikely(rc == -1))
			return sr_oom(r->e);
	}

	/* prepare for compaction */
	svmerge merge;
	sv_mergeinit(&merge);
	rc = sv_mergeprepare(&merge, r, 1 + 1);
	if (ssunlikely(rc == -1))
		return -1;
	svmergesrc *s;
	s = sv_mergeadd(&merge, &vindex_iter);

	sdcbuf *cbuf = &c->e;
	s = sv_mergeadd(&merge, NULL);
	sdreadarg arg = {
		.from_compaction     = 1,
		.io                  = &c->io,
		.index               = &node->index,
		.buf                 = &cbuf->a,
		.buf_read            = &c->d,
		.index_iter          = &cbuf->index_iter,
		.page_iter           = &cbuf->page_iter,
		.use_mmap            = index->scheme.mmap,
		.use_mmap_copy       = 0,
		.use_compression     = index->scheme.compression,
		.use_direct_io       = index->scheme.direct_io,
		.direct_io_page_size = index->scheme.direct_io_page_size,
		.compression_if      = index->scheme.compression_if,
		.has                 = 0,
		.has_vlsn            = 0,
		.o                   = SS_GTE,
		.mmap                = &node->map,
		.file                = &node->file,
		.r                   = r
	};
	ss_iterinit(sd_read, &s->src);
	rc = ss_iteropen(sd_read, &s->src, &arg, NULL);
	if (ssunlikely(rc == -1))
		return -1;
	size_stream += sd_indextotal(&node->index);

	ssiter i;
	ss_iterinit(sv_mergeiter, &i);
	ss_iteropen(sv_mergeiter, &i, r, &merge, SS_GTE);
	rc = si_merge(index, c, node, vlsn, &i, size_stream,
	              sd_indexkeys(&node->index));
	sv_mergefree(&merge, r->a);
	return rc;
}
