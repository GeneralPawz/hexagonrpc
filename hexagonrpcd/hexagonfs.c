/*
 * Virtual read-only filesystem for Hexagon processors
 *
 * Copyright (C) 2023 The Sensor Shell Contributors
 *
 * This file is part of sensh.
 *
 * Sensh is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

#include "hexagonfs.h"

static char *copy_segment_and_advance(const char *path,
				      bool *trailing_slash,
				      const char **next)
{
	const char *next_tmp;
	char *segment;
	size_t segment_len;

	next_tmp = strchr(path, '/');
	if (next_tmp == NULL) {
		next_tmp = &path[strlen(path)];
		*trailing_slash = false;
	} else {
		*trailing_slash = true;
	}

	segment_len = next_tmp - path;

	while (*next_tmp == '/')
		next_tmp++;

	segment = malloc(segment_len + 1);
	if (segment == NULL)
		return NULL;

	memcpy(segment, path, segment_len);
	segment[segment_len] = 0;

	*next = next_tmp;

	return segment;
}

static struct hexagonfs_fd *pop_dir(struct hexagonfs_fd *dir,
				    struct hexagonfs_fd *root)
{
	struct hexagonfs_fd *up;

	if (dir != root && dir->up != NULL) {
		up = dir->up;

		if (!dir->is_assigned) {
			dir->ops->close(dir->data);
			free(dir);
		}
	} else {
		up = dir;
	}

	return up;
}

static int allocate_file_number(struct hexagonfs_fd **fds,
				struct hexagonfs_fd *fd)
{
	size_t i;

	for (i = 0; i < HEXAGONFS_MAX_FD; i++) {
		if (fds[i] == NULL) {
			fd->is_assigned = true;
			fds[i] = fd;
			return i;
		}
	}

	return -EMFILE;
}

static void destroy_file_descriptor(struct hexagonfs_fd *fd)
{
	struct hexagonfs_fd *curr = fd;
	struct hexagonfs_fd *next;

	while (curr != NULL && !curr->is_assigned) {
		next = curr->up;
		curr->ops->close(curr->data);
		free(curr);

		curr = next;
	}
}

int hexagonfs_open_root(struct hexagonfs_fd **fds, struct hexagonfs_dirent *root)
{
	struct hexagonfs_fd *fd;
	int ret;

	fd = malloc(sizeof(struct hexagonfs_fd));
	if (fd == NULL)
		return -ENOMEM;

	fd->is_assigned = false;
	fd->up = NULL;
	fd->ops = root->ops;

	ret = root->ops->from_dirent(root->u.ptr, true, &fd->data);
	if (ret)
		goto err;

	ret = allocate_file_number(fds, fd);
	if (ret < 0)
		goto err;

	return ret;

err:
	destroy_file_descriptor(fd);
	return ret;
}

int hexagonfs_openat(struct hexagonfs_fd **fds, int rootfd, int dirfd, const char *name)
{
	struct hexagonfs_fd *fd;
	const char *curr = name;
	char *segment;
	bool expect_dir;
	int selected = dirfd;
	int ret = 0;

	if (*curr == '/') {
		selected = rootfd;

		while (*curr == '/')
			curr++;
	}

	fd = fds[selected];

	while (*curr != '\0' && !ret) {
		segment = copy_segment_and_advance(curr, &expect_dir, &curr);
		if (segment == NULL) {
			ret = -ENOMEM;
			goto err;
		}

		if (!strcmp(segment, ".")) {
			goto next;
		} else if (!strcmp(segment, "..")) {
			fd = pop_dir(fd, fds[rootfd]);
		} else {
			ret = fd->ops->openat(fd, segment, expect_dir, &fd);
		}

	next:
		free(segment);
	}

	if (ret)
		goto err;

	ret = allocate_file_number(fds, fd);
	if (ret)
		goto err;

	return ret;

err:
	destroy_file_descriptor(fd);

	return ret;
}

/*
 * Walks to the directory holding the last segment of the path. On success,
 * *parent is that directory and *last is a copy of the final segment, which
 * the caller frees.
 */
static int walk_to_parent(struct hexagonfs_fd **fds, int rootfd, int dirfd,
			  const char *name, struct hexagonfs_fd **parent,
			  char **last)
{
	struct hexagonfs_fd *fd;
	const char *curr = name;
	const char *next;
	char *segment;
	bool expect_dir;
	int selected = dirfd;
	int ret = 0;

	if (*curr == '/') {
		selected = rootfd;

		while (*curr == '/')
			curr++;
	}

	fd = fds[selected];

	while (true) {
		segment = copy_segment_and_advance(curr, &expect_dir, &next);
		if (segment == NULL) {
			ret = -ENOMEM;
			goto err;
		}

		if (*next == '\0')
			break;

		if (!strcmp(segment, "..")) {
			fd = pop_dir(fd, fds[rootfd]);
		} else if (strcmp(segment, ".")) {
			ret = fd->ops->openat(fd, segment, true, &fd);
		}

		free(segment);
		if (ret)
			goto err;

		curr = next;
	}

	if (segment[0] == '\0' || !strcmp(segment, ".") || !strcmp(segment, "..")) {
		free(segment);
		ret = -EINVAL;
		goto err;
	}

	*parent = fd;
	*last = segment;

	return 0;

err:
	destroy_file_descriptor(fd);

	return ret;
}

int hexagonfs_create(struct hexagonfs_fd **fds, int rootfd, int dirfd,
		     const char *name, bool append)
{
	struct hexagonfs_fd *parent, *fd;
	char *last;
	int ret;

	ret = walk_to_parent(fds, rootfd, dirfd, name, &parent, &last);
	if (ret)
		return ret;

	if (parent->ops->create == NULL)
		ret = -EROFS;
	else
		ret = parent->ops->create(parent, last, append, &fd);

	free(last);

	if (ret) {
		destroy_file_descriptor(parent);
		return ret;
	}

	ret = allocate_file_number(fds, fd);
	if (ret < 0)
		destroy_file_descriptor(fd);

	return ret;
}

int hexagonfs_unlink(struct hexagonfs_fd **fds, int rootfd, int dirfd,
		     const char *name)
{
	struct hexagonfs_fd *parent;
	char *last;
	int ret;

	ret = walk_to_parent(fds, rootfd, dirfd, name, &parent, &last);
	if (ret)
		return ret;

	if (parent->ops->unlink == NULL)
		ret = -EROFS;
	else
		ret = parent->ops->unlink(parent, last);

	free(last);
	destroy_file_descriptor(parent);

	return ret;
}

int hexagonfs_rename(struct hexagonfs_fd **fds, int rootfd, int dirfd,
		     const char *old_name, const char *new_name)
{
	struct hexagonfs_fd *old_parent, *new_parent;
	char *old_last, *new_last;
	int ret;

	ret = walk_to_parent(fds, rootfd, dirfd, old_name, &old_parent, &old_last);
	if (ret)
		return ret;

	ret = walk_to_parent(fds, rootfd, dirfd, new_name, &new_parent, &new_last);
	if (ret)
		goto err_old;

	// Both ends must be in the same kind of writable directory
	if (old_parent->ops->rename == NULL
	 || old_parent->ops->rename != new_parent->ops->rename)
		ret = -EROFS;
	else
		ret = old_parent->ops->rename(old_parent, old_last,
					      new_parent, new_last);

	free(new_last);
	destroy_file_descriptor(new_parent);
err_old:
	free(old_last);
	destroy_file_descriptor(old_parent);

	return ret;
}

int hexagonfs_close(struct hexagonfs_fd **fds, int fileno)
{
	struct hexagonfs_fd *fd;

	if (fileno < 0 || fileno >= HEXAGONFS_MAX_FD)
		return -EBADF;

	fd = fds[fileno];
	if (fd == NULL || fd->ops == NULL)
		return -EBADF;

	fd->is_assigned = false;
	destroy_file_descriptor(fd);

	fds[fileno] = NULL;

	return 0;
}

int hexagonfs_lseek(struct hexagonfs_fd **fds, int fileno, off_t off, int whence)
{
	struct hexagonfs_fd *fd;

	if (fileno < 0 || fileno >= HEXAGONFS_MAX_FD)
		return -EBADF;

	fd = fds[fileno];
	if (fd == NULL)
		return -EBADF;

	if (fd->ops->seek == NULL)
		return -ENOSYS;

	return fd->ops->seek(fd, off, whence);
}

ssize_t hexagonfs_read(struct hexagonfs_fd **fds, int fileno, size_t size, void *ptr)
{
	struct hexagonfs_fd *fd;

	if (fileno < 0 || fileno >= HEXAGONFS_MAX_FD)
		return -EBADF;

	fd = fds[fileno];
	if (fd == NULL)
		return -EBADF;

	if (fd->ops->read == NULL)
		return -ENOSYS;

	return fd->ops->read(fd, size, ptr);
}

ssize_t hexagonfs_write(struct hexagonfs_fd **fds, int fileno, size_t size, const void *ptr)
{
	struct hexagonfs_fd *fd;

	if (fileno < 0 || fileno >= HEXAGONFS_MAX_FD)
		return -EBADF;

	fd = fds[fileno];
	if (fd == NULL)
		return -EBADF;

	if (fd->ops->write == NULL)
		return -EBADF;

	return fd->ops->write(fd, size, ptr);
}

int hexagonfs_readdir(struct hexagonfs_fd **fds, int fileno, size_t ent_size, char *ent)
{
	struct hexagonfs_fd *fd;

	if (fileno < 0 || fileno >= HEXAGONFS_MAX_FD)
		return -EBADF;

	fd = fds[fileno];
	if (fd == NULL)
		return -EBADF;

	if (fd->ops->readdir == NULL)
		return -ENOSYS;

	return fd->ops->readdir(fd, ent_size, ent);
}

int hexagonfs_fstat(struct hexagonfs_fd **fds, int fileno, struct stat *stats)
{
	struct hexagonfs_fd *fd;

	if (fileno < 0 || fileno >= HEXAGONFS_MAX_FD)
		return -EBADF;

	fd = fds[fileno];
	if (fd == NULL)
		return -EBADF;

	if (fd->ops->stat == NULL)
		return -ENOSYS;

	return fd->ops->stat(fd, stats);
}
