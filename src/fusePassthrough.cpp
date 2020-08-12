/*
		Copyright (C) 2019-2020 Joshua Boudreau <jboudreau@45drives.com>
		Copyright (C) 2001-2007	Miklos Szeredi <miklos@szeredi.hu>
		Copyright (C) 2011      Sebastian Pipping <sebastian@pipping.org>
		
		This file is part of autotier.

		autotier is free software: you can redistribute it and/or modify
		it under the terms of the GNU General Public License as published by
		the Free Software Foundation, either version 3 of the License, or
		(at your option) any later version.

		autotier is distributed in the hope that it will be useful,
		but WITHOUT ANY WARRANTY; without even the implied warranty of
		MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.	See the
		GNU General Public License for more details.

		You should have received a copy of the GNU General Public License
		along with autotier.	If not, see <https://www.gnu.org/licenses/>.
*/

#define HAVE_UTIMENSAT

#include "fusePassthrough.hpp"
#include "tierEngine.hpp"
#include "file.hpp"
#include "alert.hpp"
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <dirent.h>
#include <errno.h>
#ifdef __FreeBSD__
#include <sys/socket.h>
#include <sys/un.h>
#endif
#include <sys/time.h>
#ifdef HAVE_SETXATTR
#include <sys/xattr.h>
#endif
#include <list>
#include <fuse.h>
#include <string.h>
#include <unordered_map>
#include <fstream>
#include <boost/filesystem.hpp>
namespace fs = boost::filesystem;

static sqlite3 *db;

static std::list<Tier *> tiers;

static std::unordered_map<std::string, std::string> path_cache;

fs::path _mountpoint_;

FusePassthrough::FusePassthrough(std::list<Tier> &tiers_){
	open_db();
	for(std::list<Tier>::iterator tptr = tiers_.begin(); tptr != tiers_.end(); ++tptr){
		tiers.push_back(&(*tptr));
	}
}

FusePassthrough::~FusePassthrough(){
	sqlite3_close(db);
}

void FusePassthrough::open_db(){
	int res = sqlite3_open((RUN_PATH + "/db.sqlite").c_str(), &db);
	char *errMsg = 0;
	if(res){
		std::cerr << "Error opening database: " << sqlite3_errmsg(db);
		exit(res);
	}else{
		Log("Opened database successfully", 2);
	}
	
	sqlite3_busy_timeout(db, 50);
	
	const char *sql =
	"CREATE TABLE IF NOT EXISTS Files("
	"	ID INT PRIMARY KEY NOT NULL,"
	"	RELATIVE_PATH TEXT NOT NULL,"
	"	CURRENT_TIER TEXT,"
	"	PIN TEXT,"
	"	POPULARITY REAL,"
	"	LAST_ACCESS INT"
	");";
		
	res = sqlite3_exec(db, sql, NULL, 0, &errMsg);
	
	if(res){
		std::cerr << "Error creating table: " << sqlite3_errmsg(db) << std::endl;
		std::cerr << errMsg << std::endl;
		sqlite3_free(errMsg);
		exit(res);
	}
}

// helpers

static int is_file(fs::path p){
	for(Tier *tptr : tiers){
		if(fs::is_symlink(tptr->dir / p)){
			p = fs::relative(fs::canonical(tptr->dir / p), tptr->dir);
		}
		if(fs::is_regular_file(tptr->dir / p))
			return true;
	}
	return false;
}

static int mknod_wrapper(int dirfd, const char *path, const char *link,
	int mode, dev_t rdev)
{
	int res;

	if (S_ISREG(mode)) {
		res = openat(dirfd, path, O_CREAT | O_EXCL | O_WRONLY, mode);
		if (res >= 0)
			res = close(res);
	} else if (S_ISDIR(mode)) {
		res = mkdirat(dirfd, path, mode);
	} else if (S_ISLNK(mode) && link != NULL) {
		res = symlinkat(link, dirfd, path);
	} else if (S_ISFIFO(mode)) {
		res = mkfifoat(dirfd, path, mode);
#ifdef __FreeBSD__
	} else if (S_ISSOCK(mode)) {
		struct sockaddr_un su;
		int fd;

		if (strlen(path) >= sizeof(su.sun_path)) {
			errno = ENAMETOOLONG;
			return -1;
		}
		fd = socket(AF_UNIX, SOCK_STREAM, 0);
		if (fd >= 0) {
			/*
			 * We must bind the socket to the underlying file
			 * system to create the socket file, even though
			 * we'll never listen on this socket.
			 */
			su.sun_family = AF_UNIX;
			strncpy(su.sun_path, path, sizeof(su.sun_path));
			res = bindat(dirfd, fd, (struct sockaddr*)&su,
				sizeof(su));
			if (res == 0)
				close(fd);
		} else {
			res = -1;
		}
#endif
	} else {
		res = mknodat(dirfd, path, mode, rdev);
	}

	return res;
}

static int remove_file(File *f){
	size_t ID = f->ID;
	delete f;
	std::string sql = "DELETE from Files where ID=" + std::to_string(ID);
	int res = sqlite3_exec(db, sql.c_str(), NULL, NULL, NULL);
	
	return res;
}

// methods

static int at_getattr(const char *path, struct stat *stbuf, struct fuse_file_info *fi){
  int res;
  (void) path;
  
  if(fi == NULL){
    fs::path p(path);
    if(is_file(p)){
      File f(p, db);
      res = lstat(f.old_path.c_str(), stbuf);
    }else{
      res = lstat((tiers.front()->dir / p).c_str(), stbuf);
    }
  }else{
    res = fstat(fi->fh, stbuf);
  }
  
  if (res == -1)
    return -errno;
	return 0;
}

static int at_readlink(const char *path, char *buf, size_t size){
	int res;
	
	res = readlink((tiers.front()->dir / fs::path(path)).c_str(), buf, size - 1);
  
	if (res == -1)
		return -errno;
	buf[res] = '\0';
	return 0;
}

static int at_mknod(const char *path, mode_t mode, dev_t rdev){
  int res;
	fs::path fullpath(tiers.front()->dir / path);
  
  res = mknod_wrapper(AT_FDCWD, fullpath.c_str(), NULL, mode, rdev);
  
	if (res == -1)
		return -errno;
	File(path, tiers.front(), db);
	return 0;
}

static int at_mkdir(const char *path, mode_t mode){
	int res;
	
	for(Tier *tptr : tiers){
		res = mkdir((tptr->dir / fs::path(path)).c_str(), mode);
		if (res == -1)
			return -errno;
	}
	
	return 0;
}

static int at_unlink(const char *path){
	int res;
	File *f = new File(path, db);

	res = unlink(f->old_path.c_str());
  
	if (res == -1)
		return -errno;
	return remove_file(f);
}

static int at_rmdir(const char *path){
	int res;
  
	for(Tier *tptr : tiers){
		res = rmdir((tptr->dir / fs::path(path)).c_str());
		if (res == -1)
			return -errno;
	}

	return 0;
}

static int at_symlink(const char *from, const char *to){
	int res;

	res = symlink(from, (tiers.front()->dir / fs::path(to)).c_str());
  
	if (res == -1)
		return -errno;
	return 0;
}

static int at_rename(const char *from, const char *to, unsigned int flags){
	int res;

	if (flags)
		return -EINVAL;
	
	File *f_old = new File(from, db);
	File f(from, db);
  
	fs::path from_abs(f.old_path);
	fs::path to_rel = (fs::path(to).is_absolute())? fs::relative(fs::path(to), fs::path("/")) : fs::path(to);
	fs::path to_abs = f.current_tier / to_rel;
	
	f.rel_path = to_rel;
	f.ID = std::hash<std::string>{}(to_rel.string());
	
	res = rename(from_abs.c_str(), to_abs.c_str());
  
	if (res == -1)
		return -errno;
	remove_file(f_old);
	return 0;
}

static int at_link(const char *from, const char *to){
	int res;
	File f(from, db);
  
	fs::path from_abs(f.old_path);
	fs::path to_rel = (fs::path(to).is_absolute())? fs::relative(fs::path(to), fs::path("/")) : fs::path(to);
	fs::path to_abs = f.current_tier / to_rel;
	
	f.rel_path = to_rel;
	f.ID = std::hash<std::string>{}(to_rel.string());
	
	res = link(from_abs.c_str(), to_abs.c_str());
  
  if(res == -1)
		return -errno;
	return res;
}

static int at_chmod(const char *path, mode_t mode, struct fuse_file_info *fi){
	int res;
  (void) fi;
	fs::path p(path);
  
	if(is_file(p)){
		File f(p, db);
		res = chmod(f.old_path.c_str(), mode);
	}else{
		res = chmod((tiers.front()->dir / p).c_str(), mode);
	}
	
  if (res == -1)
    return -errno;
	return res;
}

static int at_chown(const char *path, uid_t uid, gid_t gid, struct fuse_file_info *fi){
	int res;
	(void) fi;
	fs::path p(path);
	
	if(is_file(p)){
		File f(p, db);
		res = lchown(f.old_path.c_str(), uid, gid);
	}else{
		res = lchown((tiers.front()->dir / p).c_str(), uid, gid);
	}
	
  if (res == -1)
    return -errno;
	return 0;
}

static int at_truncate(const char *path, off_t size, struct fuse_file_info *fi){
	int res;

	if (fi == NULL){
    std::string backend;
    if((backend = path_cache[path+1]).empty()){
      File f(path, db);
      path_cache[path+1] = backend = f.old_path.string();
    }
		res = truncate(backend.c_str(), size);
	}else{
		res = ftruncate(fi->fh, size);
  }
  
	if (res == -1)
		return -errno;
	return 0;
}

static int at_open(const char *path, struct fuse_file_info *fi){
  int res;
	fs::path p(path);
  std::string backend;
  
	if(is_file(p)){
		if((backend = path_cache[path+1]).empty()){
      File f(path, db);
      path_cache[path+1] = backend = f.old_path.string();
    }
	}else{
    backend = (tiers.front()->dir / p).string();
	}
  res = open(backend.c_str(), fi->flags);
  
  if (res == -1)
    return -errno;
  fi->fh = res;
	return 0;
}

static int at_read(const char *path, char *buf, size_t size, off_t offset, struct fuse_file_info *fi){
	int res;
  (void) path;
  
  res = pread(fi->fh, buf, size, offset);
  
  if(res == -1)
    return -errno;
  return res;
}

static int at_write(const char *path, const char *buf, size_t size, off_t offset, struct fuse_file_info *fi){
	int res;
	(void) path;

	res = pwrite(fi->fh, buf, size, offset);
  
	if (res == -1)
		return -errno;
	return res;
}

static int at_statfs(const char *path, struct statvfs *stbuf){
	int res;
	struct statvfs fs_stats_temp;
	memset(stbuf, 0, sizeof(struct statvfs));
	
	for(Tier *tptr : tiers){
		res = statvfs((tptr->dir / fs::path(path)).c_str(), &fs_stats_temp);
		if (res == -1)
			return -errno;
		if(stbuf->f_bsize == 0) stbuf->f_bsize = fs_stats_temp.f_bsize;
		if(stbuf->f_frsize == 0) stbuf->f_frsize = fs_stats_temp.f_frsize;
		stbuf->f_blocks += fs_stats_temp.f_blocks;
		stbuf->f_bfree += fs_stats_temp.f_bfree;
		stbuf->f_bavail += fs_stats_temp.f_bavail;
		stbuf->f_files += fs_stats_temp.f_files;
		if(stbuf->f_ffree == 0) stbuf->f_ffree = fs_stats_temp.f_ffree;
		if(stbuf->f_favail == 0) stbuf->f_favail = fs_stats_temp.f_favail;
	}

	return 0;
}

static int at_flush(const char *path, struct fuse_file_info *fi)
{
	int res;
	(void) path;
  
	/* This is called from every close on an open file, so call the
	   close on the underlying filesystem.	But since flush may be
	   called multiple times for an open file, this must not really
	   close the file.  This is important if used on a network
	   filesystem like NFS which flush the data/metadata on close() */
  
	res = close(dup(fi->fh));
  
	if (res == -1)
		return -errno;
	return 0;
}


static int at_release(const char *path, struct fuse_file_info *fi){
	close(fi->fh);
  path_cache.erase(path+1);
	return 0;
}

static int at_fsync(const char *path, int isdatasync, struct fuse_file_info *fi){
	int res;
	(void) path;

#ifndef HAVE_FDATASYNC
	(void) isdatasync;
#else
	if (isdatasync)
		res = fdatasync(fi->fh);
	else
#endif
		res = fsync(fi->fh);
  
	if (res == -1)
		return -errno;
	return 0;
}

#ifdef HAVE_SETXATTR
static int at_setxattr(const char *path, const char *name, const char *value, size_t size, int flags){
	
}

static int at_getxattr(const char *path, const char *name, char *value, size_t size){
	
}

static int at_listxattr(const char *path, char *list, size_t size){
	
}

static int at_removexattr(const char *path, const char *name){
	
}

#endif
static int at_readdir(
	const char *path, void *buf, fuse_fill_dir_t filler, off_t offset, struct fuse_file_info *fi,
	enum fuse_readdir_flags flags
){
	DIR *dp;
	struct dirent *de;

	(void) offset;
	(void) fi;
	(void) flags;
	for(Tier *tptr : tiers){
		dp = opendir((tptr->dir / fs::path(path)).c_str());
		if (dp == NULL)
			return -errno;

		while ((de = readdir(dp)) != NULL) {
			if(de->d_type == DT_DIR && tptr != tiers.front())
				continue;
			struct stat st;
			memset(&st, 0, sizeof(st));
			st.st_ino = de->d_ino;
			st.st_mode = de->d_type << 12;
			if (filler(buf, de->d_name, &st, 0, (fuse_fill_dir_flags)0))
				break;
		}

		closedir(dp);
	}
	return 0;
}

void *at_init(struct fuse_conn_info *conn, struct fuse_config *cfg){
	(void) conn;
	cfg->use_ino = 1;

	/* Pick up changes from lower filesystem right away. This is
		 also necessary for better hardlink support. When the kernel
		 calls the unlink() handler, it does not know the inode of
		 the to-be-removed entry and can therefore not invalidate
		 the cache of the associated inode - resulting in an
		 incorrect st_nlink value being reported for any remaining
		 hardlinks to this inode. */
  
	cfg->entry_timeout = 0;
	cfg->attr_timeout = 0;
	cfg->negative_timeout = 0;

	return NULL;
}

static int at_access(const char *path, int mask){
  int res;
	fs::path p(path);
  
	if(is_file(p)){
		File f(p, db);
		res = access(f.old_path.c_str(), mask);
	}else{
		res = access((tiers.front()->dir / p).c_str(), mask);
	}
	
  if (res == -1)
    return -errno;
	return 0;
}

static int at_create(const char *path, mode_t mode, struct fuse_file_info *fi){
	int res;
	fs::path fullpath(tiers.front()->dir / path);
  
	res = open(fullpath.c_str(), fi->flags, mode);
  
	if (res == -1)
		return -errno;
	File(path, tiers.front(), db);
	fi->fh = res;
	return 0;
}

#ifdef HAVE_UTIMENSAT
static int at_utimens(const char *path, const struct timespec ts[2], struct fuse_file_info *fi){
	int res;
	File f(path, db);
	(void) fi;
	
	/* don't use utime/utimes since they follow symlinks */
	res = utimensat(0, f.old_path.c_str(), ts, AT_SYMLINK_NOFOLLOW);
  
	if (res == -1)
		return -errno;
	return 0;
}

#endif

static int at_write_buf(const char *path, struct fuse_bufvec *buf, off_t offset, struct fuse_file_info *fi){
	struct fuse_bufvec dst = FUSE_BUFVEC_INIT(fuse_buf_size(buf));

	(void) path;

	dst.buf[0].flags = (fuse_buf_flags)(FUSE_BUF_IS_FD | FUSE_BUF_FD_SEEK);
	dst.buf[0].fd = fi->fh;
	dst.buf[0].pos = offset;

	return fuse_buf_copy(&dst, buf, FUSE_BUF_SPLICE_NONBLOCK);
}

static int at_read_buf(const char *path, struct fuse_bufvec **bufp, size_t size, off_t offset, struct fuse_file_info *fi){
	struct fuse_bufvec *src;

	(void) path;

	src = (struct fuse_bufvec *)malloc(sizeof(struct fuse_bufvec));
	if (src == NULL)
		return -ENOMEM;

	*src = FUSE_BUFVEC_INIT(size);

	src->buf[0].flags = (fuse_buf_flags)(FUSE_BUF_IS_FD | FUSE_BUF_FD_SEEK);
	src->buf[0].fd = fi->fh;
	src->buf[0].pos = offset;

	*bufp = src;

	return 0;
}

#ifdef HAVE_POSIX_FALLOCATE
static int at_fallocate(const char *path, int mode, off_t offset, off_t length, struct fuse_file_info *fi){
	(void) path;

	if (mode)
		return -EOPNOTSUPP;

	return -posix_fallocate(fi->fh, offset, length);
}

#endif
#ifdef HAVE_COPY_FILE_RANGE
static ssize_t at_copy_file_range(
	const char *path_in, struct fuse_file_info *fi_in, off_t offset_in, const char *path_out,
	struct fuse_file_info *fi_out, off_t offset_out, size_t len, int flags
){
	
}

#endif
static off_t at_lseek(const char *path, off_t off, int whence, struct fuse_file_info *fi){
	int fd;
	off_t res;

	if (fi == NULL){
		File f(path, db);
		fd = open(f.old_path.c_str(), O_RDONLY);
	}else
		fd = fi->fh;

	if (fd == -1)
		return -errno;

	res = lseek(fd, off, whence);
	if (res == -1)
		res = -errno;

	if (fi == NULL)
		close(fd);
	return res;
}


int FusePassthrough::mount_fs(fs::path mountpoint, char *fuse_opts){
	_mountpoint_ = mountpoint; // global
	Log("Mounting filesystem", 2);
	static const struct fuse_operations at_oper = {
		.getattr					= at_getattr,
		.readlink					= at_readlink,
		.mknod						= at_mknod,
		.mkdir						= at_mkdir,
		.unlink						= at_unlink,
		.rmdir						= at_rmdir,
		.symlink					= at_symlink,
		.rename						= at_rename,
		.link							= at_link,
		.chmod						= at_chmod,
		.chown						= at_chown,
		.truncate					= at_truncate,
		.open							= at_open,
		.read							= at_read,
		.write						= at_write,
		.statfs						= at_statfs,
    .flush            = at_flush,
		.release					= at_release,
		.fsync						= at_fsync,
#ifdef HAVE_SETXATTR
		.setxattr					= at_setxattr,
		.getxattr					= at_getxattr,
		.listxattr				= at_listxattr,
		.removexattr			= at_removexattr,
#endif
		.readdir					= at_readdir,
		.init			 				= at_init,
		.access						= at_access,
		.create 					= at_create,
#ifdef HAVE_UTIMENSAT
		.utimens					= at_utimens,
#endif
    .write_buf        = at_write_buf,
    .read_buf         = at_read_buf,
#ifdef HAVE_POSIX_FALLOCATE
		.fallocate				= at_fallocate,
#endif
#ifdef HAVE_COPY_FILE_RANGE
		.copy_file_range 	= at_copy_file_range,
#endif
		.lseek						= at_lseek,
	};
	//std::cerr << std::string("\"") + std::string(mountpoint_) + std::string("\"") << std::endl;
	std::vector<char *>argv = {strdup("autotier"), strdup(mountpoint.c_str())};
  if(fuse_opts){
    argv.push_back(strdup("-o"));
    argv.push_back(fuse_opts);
  }
	return fuse_main(argv.size(), argv.data(), &at_oper, NULL);
}
