
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

int sc_backupstart(sc *s)
{
	/* begin backup procedure
	 * state 0
	 *
	 * disable log garbage-collection
	*/
	sw_managergc_enable(s->wm, 0);
	ss_mutexlock(&s->lock);
	if (ssunlikely(s->backup > 0)) {
		ss_mutexunlock(&s->lock);
		sw_managergc_enable(s->wm, 1);
		/* in progress */
		return 1;
	}
	uint64_t bsn = sr_seq(s->r->seq, SR_BSNNEXT);
	s->backup = 1;
	s->backup_bsn = bsn;
	ss_mutexunlock(&s->lock);
	return 0;
}

int sc_backupbegin(sc *s)
{
	/*
	 * a. create backup_path/<bsn.incomplete> directory
	 * b. create database directories
	 * c. create log directory
	*/
	char path[1024];
	snprintf(path, sizeof(path), "%s/%" PRIu32 ".incomplete",
	         s->backup_path, s->backup_bsn);
	int rc = ss_vfsmkdir(s->r->vfs, path, 0755);
	if (ssunlikely(rc == -1)) {
		sr_error(s->r->e, "backup directory '%s' create error: %s",
		         path, strerror(errno));
		return -1;
	}
	int i = 0;
	while (i < s->count) {
		scdb *db = &s->i[i];
		snprintf(path, sizeof(path), "%s/%" PRIu32 ".incomplete/%s",
		         s->backup_path, s->backup_bsn,
		         db->index->scheme.name);
		rc = ss_vfsmkdir(s->r->vfs, path, 0755);
		if (ssunlikely(rc == -1)) {
			sr_error(s->r->e, "backup directory '%s' create error: %s",
			         path, strerror(errno));
			return -1;
		}
		i++;
	}
	snprintf(path, sizeof(path), "%s/%" PRIu32 ".incomplete/log",
	         s->backup_path, s->backup_bsn);
	rc = ss_vfsmkdir(s->r->vfs, path, 0755);
	if (ssunlikely(rc == -1)) {
		sr_error(s->r->e, "backup directory '%s' create error: %s",
		         path, strerror(errno));
		return -1;
	}

	ss_mutexlock(&s->lock);
	s->backup = 2;
	s->backup_in_progress = s->count;
	i = 0;
	while (i < s->count) {
		sc_task_backup(&s->i[i]);
		i++;
	}
	ss_mutexunlock(&s->lock);
	return 0;
}

int sc_backupend(sc *s, scworker *w)
{
	/*
	 * a. rotate log file
	 * b. copy log files
	 * c. enable log gc
	 * d. rename <bsn.incomplete> into <bsn>
	 * e. set last backup, set COMPLETE
	 */

	/* force log rotation */
	ss_trace(&w->trace, "%s", "log rotation for backup");
	int rc = sw_managerrotate(s->wm);
	if (ssunlikely(rc == -1))
		return -1;

	/* copy log files */
	ss_trace(&w->trace, "%s", "log files backup");

	char path[1024];
	snprintf(path, sizeof(path), "%s/%" PRIu32 ".incomplete/log",
	         s->backup_path, s->backup_bsn);
	rc = sw_managercopy(s->wm, path, &w->dc.c);
	if (ssunlikely(rc == -1))
		return -1;

	/* complete backup */
	snprintf(path, sizeof(path), "%s/%" PRIu32 ".incomplete",
	         s->backup_path, s->backup_bsn);
	char newpath[1024];
	snprintf(newpath, sizeof(newpath), "%s/%" PRIu32,
	         s->backup_path, s->backup_bsn);
	rc = ss_vfsrename(s->r->vfs, path, newpath);
	if (ssunlikely(rc == -1)) {
		sr_error(s->r->e, "backup directory '%s' rename error: %s",
		         path, strerror(errno));
		return -1;
	}

	/* enable log gc */
	sw_managergc_enable(s->wm, 1);

	/* complete */
	ss_mutexlock(&s->lock);
	s->backup_bsn_last = s->backup_bsn;
	s->backup_bsn_last_complete = 1;
	s->backup_in_progress = 0;
	s->backup = 0;
	s->backup_bsn = 0;
	ss_mutexunlock(&s->lock);
	return 0;
}

int sc_backupstop(sc *s)
{
	sw_managergc_enable(s->wm, 1);
	ss_mutexlock(&s->lock);
	s->backup = 0;
	s->backup_bsn_last_complete = 0;
	ss_mutexunlock(&s->lock);
	return 0;
}
