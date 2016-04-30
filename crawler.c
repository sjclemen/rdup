/*
 * Copyright (c) 2005 - 2011 Miek Gieben
 * License: GPLv3(+), see LICENSE for details
 *
 * Directory crawler
 */
#include "rdup.h"
#ifdef HAVE_LIBNETTLE
#include <nettle/sha.h>
#else
#define SHA1_DIGEST_SIZE 20
#endif				/* HAVE_LIBNETTLE */

extern gboolean opt_onefilesystem;
extern gboolean opt_nobackup;
extern gboolean opt_chown;
extern time_t opt_timestamp;
extern gint opt_verbose;
extern GSList *pregex_list;

/* sha1.c */
int sha1_stream(FILE * stream, void *digest);

/* common.c */
struct rdup *entry_dup(struct rdup *);
void entry_free(struct rdup *);

/**
 * prepend path leading up to backup directory to the tree
 */
gboolean dir_prepend(GTree * t, char *path, GHashTable * u, GHashTable * g)
{
	char *c;
	char *p;
	char *path2;
	size_t len;
	struct stat s;
	struct rdup e;

	path2 = g_strdup(path);
	len = strlen(path);

	/* add closing / */
	if (path2[len - 1] != '/') {
		path2 = g_realloc(path2, len + 2);
		path2[len] = '/';
		path2[len + 1] = '\0';
	}

	for (p = path2 + 1; (c = strchr(p, '/')); p++) {
		*c = '\0';
		if (lstat(path2, &s) != 0) {
			msg(_("Could not stat path `%s\': %s"), path2,
			    strerror(errno));
			g_free(path2);
			return FALSE;
		}
		e.f_name = path2;
		e.f_target = NULL;
		e.f_name_size = strlen(path2);
		e.f_uid = s.st_uid;
		e.f_user = lookup_user(u, e.f_uid);
		e.f_gid = s.st_gid;
		e.f_group = lookup_group(g, e.f_gid);
		e.f_ctime = s.st_ctime;
		e.f_mtime = s.st_mtime;
		e.f_atime = s.st_atime;
		e.f_mode = s.st_mode;
		e.f_size = s.st_size;
		e.f_dev = s.st_dev;
		e.f_rdev = s.st_rdev;
		e.f_ino = s.st_ino;
		e.f_lnk = 0;
		e.f_hash = NULL;

		/* symlinks; also set the target */
		if (S_ISLNK(s.st_mode)) {
			e.f_target = slink(&e);
			e.f_size = e.f_name_size;
			e.f_name_size += 4 + strlen(e.f_target);
			/* When we encounter a symlink on this level, it is very hard to make this
			 * backup work, because the target may fall out of the backup. If this
			 * is the case the entire backup fails. Gnu tar only show the symlink
			 * and then stops. We do now the same, heance the return FALSE
			 */
			g_tree_insert(t, (gpointer) entry_dup(&e), VALUE);
			g_free(e.f_target);
			g_free(path2);
			return FALSE;
		}

		g_tree_insert(t, (gpointer) entry_dup(&e), VALUE);
		*c = '/';
		p = c++;
	}
	g_free(path2);
	return TRUE;
}

/*
 * calculates a files sha1 sum
 */
static gboolean sha1_str(char *digest_out,
		     char *filename)
{
#ifdef HAVE_LIBNETTLE
	unsigned char digest[SHA1_DIGEST_SIZE];
	gint i;
	FILE *file;

	if ((file = fopen(filename, "r")) == NULL) {
		msg(_("Could not open '%s\': %s"), filename, strerror(errno));
		return FALSE;
	}
	if (sha1_stream(file, digest) != 0) {
		msg(_("Failed to calculate sha1 digest: `%s\'"), filename);
		fclose(file);
		return FALSE;
	}
	fclose(file);
	for (i = 0; i < SHA1_DIGEST_SIZE; i++) {
		snprintf(digest_out + i*2, 3, "%02x", digest[i]);
	}
#else
	snprintf(digest_out, SHA1_DIGEST_SIZE*2, "%s", NO_SHA);
#endif				/* HAVE_LIBNETTLE */
	return TRUE;
}


void
dir_crawl(GTree * t, GHashTable * linkhash, GHashTable * userhash,
	  GHashTable * grouphash, char *path)
{
	DIR *dir;
	struct dirent *dent;
	struct rdup *directory;
	struct chown_pack *cp;
	char *curpath;
	gchar *lnk;
	struct stat s;
	struct rdup pop;
	struct remove_path rp;
	dev_t current_dev;
	size_t curpath_len;
	char sha1sum_str[SHA1_DIGEST_SIZE*2+1];
	sha1sum_str[SHA1_DIGEST_SIZE*2] = '\0';

	/* dir stack */
	gint32 d = 0;
	gint32 dstack_cnt = 1;
	struct rdup **dirstack =
	    g_malloc(dstack_cnt * D_STACKSIZE * sizeof(struct rdup *));

	if (!(dir = opendir(path))) {
		/* non-dirs are also allowed, check for this, if it isn't give the error */
		if (access(path, R_OK) == 0) {
			g_free(dirstack);
			return;
		}
		msg(_("Cannot enter directory `%s\': %s"), path,
		    strerror(errno));
		g_free(dirstack);
		return;
	}

	/* get device */
#ifdef HAVE_DIRFD
	if (fstat(dirfd(dir), &s) != 0) {
#else
	if (fstat(rdup_dirfd(dir), &s) != 0) {
#endif
		msg(_
		    ("Cannot determine holding device of the directory `%s\': %s"),
		    path, strerror(errno));
		closedir(dir);
		g_free(dirstack);
		return;
	}
	current_dev = s.st_dev;

	while ((dent = readdir(dir))) {
		if (!strcmp(dent->d_name, ".") || !strcmp(dent->d_name, ".."))
			continue;

		if (opt_chown) {
			if (!strncmp(dent->d_name, USRGRPINFO, LEN_USRGRPINFO)) {
				continue;
			}
		}

		if (strcmp(path, "/") == 0) {
			curpath = g_strdup_printf("/%s", dent->d_name);
			curpath_len = strlen(curpath);
		} else {
			curpath = g_strdup_printf("%s/%s", path, dent->d_name);
			curpath_len = strlen(curpath);
		}

		if (lstat(curpath, &s) != 0) {
			msg(_("Could not stat path `%s\': %s"), curpath,
			    strerror(errno));
			g_free(curpath);
			continue;
		}

		if (strchr(curpath, '\n')) {
			msg(_("Newline (\\n) found in path `%s\', skipping"),
			    curpath);
			g_free(curpath);
			continue;
		}

		if (S_ISREG(s.st_mode) || S_ISLNK(s.st_mode) ||
		    S_ISBLK(s.st_mode) || S_ISCHR(s.st_mode) ||
		    S_ISFIFO(s.st_mode) || S_ISSOCK(s.st_mode)) {

			pop.f_name = curpath;
			pop.f_target = NULL;
			pop.f_name_size = curpath_len;
			pop.f_uid = s.st_uid;
			pop.f_user = lookup_user(userhash, pop.f_uid);
			pop.f_gid = s.st_gid;
			pop.f_group = lookup_group(grouphash, pop.f_gid);
			pop.f_ctime = s.st_ctime;
			pop.f_mtime = s.st_mtime;
			pop.f_atime = s.st_atime;
			pop.f_mode = s.st_mode;
			pop.f_size = s.st_size;
			pop.f_dev = s.st_dev;
			pop.f_rdev = s.st_rdev;
			pop.f_ino = s.st_ino;
			pop.f_lnk = 0;

			if (gfunc_regexp(pregex_list, curpath, curpath_len)) {
				g_free(curpath);
				continue;
			}

			/* hardlinks */
			if (s.st_nlink > 1) {
				if ((lnk = hlink(linkhash, &pop))) {
					pop.f_target = lnk;
					pop.f_lnk = 1;
				}
			}

			pop.f_hash = NULL;
			if (S_ISREG(s.st_mode)) {
				if (sha1_str((char*)(&sha1sum_str), curpath)) {
					pop.f_hash = strdup(sha1sum_str);
				}
			}

			if (S_ISLNK(s.st_mode))
				pop.f_target = slink(&pop);

			if (S_ISLNK(s.st_mode) || pop.f_lnk) {
				/* fix the name and the sizes */
				pop.f_size = pop.f_name_size;
				pop.f_name_size += 4 + strlen(pop.f_target);
			}
			/* check for USRGRPINFO file */
			if (opt_chown
			    && (cp = chown_parse(path, dent->d_name)) != NULL) {
				pop.f_uid = cp->u;
				pop.f_gid = cp->g;
				pop.f_user = cp->user;
				pop.f_group = cp->group;
			}

			if (opt_nobackup && !strcmp(dent->d_name, NOBACKUP)) {
				/* return after seeing .nobackup */
				if (opt_verbose > 0) {
					msg(_("%s found in '%s\'"), NOBACKUP,
					    path);
				}
				/* remove all files found in this path */
				rp.tree = t;
				rp.len = strlen(path);
				rp.path = path;
				g_tree_foreach(t, gfunc_remove_path,
					       (gpointer) & rp);
				/* add .nobackup back in */
				g_tree_insert(t, (gpointer) entry_dup(&pop),
					      VALUE);
				g_free(dirstack);
				closedir(dir);
				return;
			}

			g_tree_insert(t, (gpointer) entry_dup(&pop), VALUE);

			if (pop.f_target != NULL)
				g_free(pop.f_target);
			g_free(curpath);
			continue;
		} else if (S_ISDIR(s.st_mode)) {
			/* one filesystem */
			if (opt_onefilesystem && s.st_dev != current_dev) {
				msg(_
				    ("Not walking into different filesystem `%s\'"),
				    curpath);
				g_free(curpath);
				continue;
			}
			/* Exclude list */
			if (gfunc_regexp(pregex_list, curpath, curpath_len)) {
				g_free(curpath);
				continue;
			}
			dirstack[d] = g_malloc(sizeof(struct rdup));
			dirstack[d]->f_name = g_strdup(curpath);
			dirstack[d]->f_target = NULL;
			dirstack[d]->f_name_size = curpath_len;
			dirstack[d]->f_uid = s.st_uid;
			dirstack[d]->f_user = lookup_user(userhash, s.st_uid);
			dirstack[d]->f_gid = s.st_gid;
			dirstack[d]->f_group =
			    lookup_group(grouphash, s.st_gid);
			dirstack[d]->f_ctime = s.st_ctime;
			dirstack[d]->f_mtime = s.st_mtime;
			dirstack[d]->f_atime = s.st_atime;
			dirstack[d]->f_mode = s.st_mode;
			dirstack[d]->f_size = s.st_size;
			dirstack[d]->f_dev = s.st_dev;
			dirstack[d]->f_rdev = s.st_rdev;
			dirstack[d]->f_ino = s.st_ino;
			dirstack[d]->f_lnk = 0;
			dirstack[d]->f_hash = NULL;

			/* check for USRGRPINFO file */
			if (opt_chown
			    && (cp = chown_parse(curpath, NULL)) != NULL) {
				dirstack[d]->f_uid = cp->u;
				dirstack[d]->f_gid = cp->g;
				dirstack[d]->f_user = cp->user;
				dirstack[d]->f_group = cp->group;
			}

			if (d++ % D_STACKSIZE == 0) {
				dirstack = g_realloc(dirstack,
						     ++dstack_cnt *
						     D_STACKSIZE *
						     sizeof(struct rdup *));
			}
			g_free(curpath);
			continue;
		} else {
			if (opt_verbose > 0) {
				msg(_("Neither file nor directory `%s\'"),
				    curpath);
			}
			g_free(curpath);
		}
	}
	closedir(dir);

	while (d > 0) {
		directory = dirstack[--d];
		g_tree_insert(t, (gpointer) entry_dup(directory), VALUE);
		/* recurse */
		/* potentially expensive operation. Better would be to when we hit
		 * .nobackup to go up the tree and delete some nodes.... or not */
		dir_crawl(t, linkhash, userhash, grouphash, directory->f_name);
		entry_free(directory);
	}
	g_free(dirstack);
	return;
}
