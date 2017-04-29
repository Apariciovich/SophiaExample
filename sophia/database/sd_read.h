#ifndef SD_READ_H_
#define SD_READ_H_

/*
 * sophia database
 * sphia.org
 *
 * Copyright (c) Dmitry Simonenko
 * BSD License
*/

typedef struct sdread sdread;
typedef struct sdreadarg sdreadarg;

struct sdreadarg {
	sdio       *io;
	sdindex    *index;
	ssbuf      *buf;
	ssbuf      *buf_read;
	ssiter     *index_iter;
	ssiter     *page_iter;
	ssmmap     *mmap;
	ssfile     *file;
	ssorder     o;
	int         from_compaction;
	int         has;
	uint64_t    has_vlsn;
	int         use_mmap;
	int         use_mmap_copy;
	int         use_compression;
	int         use_direct_io;
	int         direct_io_page_size;
	ssfilterif *compression_if;
	sr         *r;
};

struct sdread {
	sdreadarg    ra;
	sdindexpage *ref;
	sdpage       page;
	int          reads;
} sspacked;

static inline int
sd_read_page(sdread *i, sdindexpage *ref)
{
	sdreadarg *arg = &i->ra;
	sr *r = arg->r;

	int page_align = arg->io->size_page * 4;
	i->reads++;

	ss_bufreset(arg->buf);
	int rc = ss_bufensure(arg->buf, r->a, ref->sizeorigin + page_align);
	if (ssunlikely(rc == -1))
		return sr_oom(r->e);

	/* compression */
	char *page_pointer;
	if (arg->use_compression)
	{
		if (arg->use_mmap) {
			page_pointer = arg->mmap->p + ref->offset;
		} else {
			ss_bufreset(arg->buf_read);
			rc = ss_bufensure(arg->buf_read, r->a, ref->size + page_align);
			if (ssunlikely(rc == -1))
				return sr_oom(r->e);
			rc = sd_ioread(arg->io, r, arg->file, ref->offset,
			               arg->buf_read->s, ref->size,
			               arg->from_compaction,
			               &page_pointer);
			if (ssunlikely(rc == -1))
				return -1;
			ss_bufadvance(arg->buf_read, ref->size);
		}

		/* copy header */
		memcpy(arg->buf->p, page_pointer, sizeof(sdpageheader));
		ss_bufadvance(arg->buf, sizeof(sdpageheader));

		/* decompression */
		ssfilter f;
		rc = ss_filterinit(&f, (ssfilterif*)arg->compression_if, r->a, SS_FOUTPUT);
		if (ssunlikely(rc == -1)) {
			sr_error(r->e, "db file '%s' decompression error",
			         ss_pathof(&arg->file->path));
			return -1;
		}
		int size = ref->size - sizeof(sdpageheader);
		rc = ss_filternext(&f, arg->buf, page_pointer + sizeof(sdpageheader), size);
		if (ssunlikely(rc == -1)) {
			sr_error(r->e, "db file '%s' decompression error",
			         ss_pathof(&arg->file->path));
			return -1;
		}
		ss_filterfree(&f);
		sd_pageinit(&i->page, (sdpageheader*)arg->buf->s);
		return 0;
	}

	/* mmap */
	if (arg->use_mmap) {
		if (arg->use_mmap_copy) {
			memcpy(arg->buf->s, arg->mmap->p + ref->offset, ref->sizeorigin);
			sd_pageinit(&i->page, (sdpageheader*)(arg->buf->s));
		} else {
			sd_pageinit(&i->page, (sdpageheader*)(arg->mmap->p + ref->offset));
		}
		return 0;
	}

	/* default */
	rc = sd_ioread(arg->io, r, arg->file, ref->offset,
	               arg->buf->s, ref->size,
	               arg->from_compaction,
	               &page_pointer);
	if (ssunlikely(rc == -1))
		return -1;
	ss_bufadvance(arg->buf, ref->size);
	sd_pageinit(&i->page, (sdpageheader*)page_pointer);
	return 0;
}

static inline int
sd_read_openpage(sdread *i, char *key)
{
	sdreadarg *arg = &i->ra;
	assert(i->ref != NULL);
	int rc = sd_read_page(i, i->ref);
	if (ssunlikely(rc == -1))
		return -1;
	ss_iterinit(sd_pageiter, arg->page_iter);
	return ss_iteropen(sd_pageiter, arg->page_iter, arg->r,
	                   &i->page, arg->o, key);
}

static inline void
sd_read_next(ssiter*);

static inline int
sd_read_open(ssiter *iptr, sdreadarg *arg, char *key)
{
	sdread *i = (sdread*)iptr->priv;
	i->reads = 0;
	i->ra = *arg;
	ss_iterinit(sd_indexiter, arg->index_iter);
	ss_iteropen(sd_indexiter, arg->index_iter, arg->r, arg->index,
	            arg->o, key);
	i->ref = ss_iterof(sd_indexiter, arg->index_iter);
	if (i->ref == NULL)
		return 0;
	if (arg->has) {
		assert(arg->o == SS_GTE);
		if (sslikely(i->ref->lsnmax <= arg->has_vlsn)) {
			i->ref = NULL;
			return 0;
		}
	}
	int rc = sd_read_openpage(i, key);
	if (ssunlikely(rc == -1)) {
		i->ref = NULL;
		return -1;
	}
	if (ssunlikely(! ss_iterhas(sd_pageiter, i->ra.page_iter))) {
		sd_read_next(iptr);
		rc = 0;
	}
	return rc;
}

static inline void
sd_read_close(ssiter *iptr)
{
	sdread *i = (sdread*)iptr->priv;
	i->ref = NULL;
}

static inline int
sd_read_has(ssiter *iptr)
{
	sdread *i = (sdread*)iptr->priv;
	if (ssunlikely(i->ref == NULL))
		return 0;
	return ss_iterhas(sd_pageiter, i->ra.page_iter);
}

static inline void*
sd_read_of(ssiter *iptr)
{
	sdread *i = (sdread*)iptr->priv;
	if (ssunlikely(i->ref == NULL))
		return NULL;
	return ss_iterof(sd_pageiter, i->ra.page_iter);
}

static inline void
sd_read_next(ssiter *iptr)
{
	sdread *i = (sdread*)iptr->priv;
	if (ssunlikely(i->ref == NULL))
		return;
	ss_iternext(sd_pageiter, i->ra.page_iter);
retry:
	if (sslikely(ss_iterhas(sd_pageiter, i->ra.page_iter)))
		return;
	ss_iternext(sd_indexiter, i->ra.index_iter);
	i->ref = ss_iterof(sd_indexiter, i->ra.index_iter);
	if (i->ref == NULL)
		return;
	int rc = sd_read_openpage(i, NULL);
	if (ssunlikely(rc == -1)) {
		i->ref = NULL;
		return;
	}
	goto retry;
}

static inline int
sd_read_stat(ssiter *iptr)
{
	sdread *i = (sdread*)iptr->priv;
	return i->reads;
}

extern ssiterif sd_read;

#endif
