#include <assert.h>
#include <dirent.h>
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <openssl/md5.h>
#include <openssl/sha.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "sqlite3.h"

#define MAX_STACK_SIZE 128
#define BUFFER_SIZE   4096

#define PN_PARENT        1
#define PN_TYPE          2
#define PN_NAME          3
#define PN_SIZE          4
#define PN_ATIME         5
#define PN_MTIME         6
#define PN_CTIME         7
#define PN_ERROR         8
#define PN_MD5           9
#define PN_SHA1         10
#define PN_SHA256       11
#define PN_SHA512       12

#define AR_DATABASE      1
#define AR_TRANSACTION   2
#define AR_PARENT        3
#define AR_NAME          4
#define AR_PATH          5
#define AR_COUNT         6

sqlite3 *db;        // database handle
sqlite3_stmt *stmt; // INSERT statement

static void cleanup_db()
{
	int status = sqlite3_close(db);

	if (status != SQLITE_OK) {
		warnx("sqlite3_close failed with status %d: %s", status, sqlite3_errmsg(db));
	}
}

static void cleanup_stmt()
{
	int status = sqlite3_finalize(stmt);

	if (status != SQLITE_OK) {
		warnx("sqlite3_finalize failed with status %d: %s", status, sqlite3_errmsg(db));
	}
}

int main(int argc, char *argv[])
{
	if (argc != AR_COUNT) {
		puts(
			"\targv[1] = database file name\n"
			"\targv[2] = whether to use transaction\n"
			"\targv[3] = parent node id or zero for no parent\n"
			"\targv[4] = node name\n"
			"\targv[5] = directory path"
		);
		exit(EXIT_SUCCESS);
	}

	int status = sqlite3_open(argv[AR_DATABASE], &db);

	atexit(cleanup_db);

	if (status != SQLITE_OK) {
		errx(EXIT_FAILURE, "Could not open database: %s", sqlite3_errmsg(db));
	}

	if (sqlite3_db_config(db, SQLITE_DBCONFIG_ENABLE_FKEY, 1, &status) != SQLITE_OK) {
		errx(EXIT_FAILURE, "Could not enforce foreign keys: %s", sqlite3_errmsg(db));
	}

	if (!status) {
		errx(EXIT_FAILURE, "Could not enforce foreign keys");
	}

	char *sqlerr;

	if (sqlite3_exec(db, "PRAGMA recursive_triggers = ON", NULL, NULL, &sqlerr) != SQLITE_OK) {
		warnx("Could not enable recursive triggers: %s", sqlerr);
		sqlite3_free(sqlerr);
		exit(EXIT_FAILURE);
	}

	int transaction = atoi(argv[AR_TRANSACTION]);

	if (transaction && sqlite3_exec(db, "BEGIN", NULL, NULL, &sqlerr) != SQLITE_OK) {
		warnx("Could not begin transaction: %s", sqlerr);
		sqlite3_free(sqlerr);
		exit(EXIT_FAILURE);
	}

	if (sqlite3_prepare(db, "INSERT INTO nodes(parent,type,name,size,atime,mtime,ctime,error,md5,sha1,sha256,sha512) VALUES (?,?,?,?,?,?,?,?,?,?,?,?)", -1, &stmt, NULL) != SQLITE_OK) {
		errx(EXIT_FAILURE, "Could not prepare statement: %s\n", sqlite3_errmsg(db));
	}

	atexit(cleanup_stmt);

	// state structure

	struct {
		DIR *dir;
		sqlite3_int64 row;
		size_t pos;
	} current, stack[MAX_STACK_SIZE];

	// open main directory

	current.dir = opendir(argv[AR_PATH]);

	if (!current.dir) {
		err(EXIT_FAILURE, "Could not open main directory");
	}

	// insert main entry

	long long int id = strtoll(argv[AR_PARENT], NULL, 10);

	if (id) {
		assert(SQLITE_OK == sqlite3_bind_int64(stmt, PN_PARENT, id));
	} else {
		assert(SQLITE_OK == sqlite3_bind_null(stmt, PN_PARENT));
	}

	assert(SQLITE_OK == sqlite3_bind_int(stmt, PN_TYPE, DT_DIR));
	assert(SQLITE_OK == sqlite3_bind_text(stmt, PN_NAME, argv[AR_NAME], -1, SQLITE_STATIC));
	//sqlite3_bind_null(stmt, PN_SIZE);
	//sqlite3_bind_null(stmt, PN_ERROR);
	assert(SQLITE_OK == sqlite3_bind_int64(stmt, PN_SIZE, 0LL));
	assert(SQLITE_OK == sqlite3_bind_int(stmt, PN_ERROR, 0));
	assert(SQLITE_OK == sqlite3_bind_null(stmt, PN_MD5));
	assert(SQLITE_OK == sqlite3_bind_null(stmt, PN_SHA1));
	assert(SQLITE_OK == sqlite3_bind_null(stmt, PN_SHA256));
	assert(SQLITE_OK == sqlite3_bind_null(stmt, PN_SHA512));

	struct stat sb;

	if (lstat(argv[AR_PATH], &sb)) { // like stat, except does not resolve symlinks
		warn("Could not stat main directory");
		//sqlite3_bind_null(stmt, PN_SIZE);
		assert(SQLITE_OK == sqlite3_bind_int64(stmt, PN_ATIME, 0LL));
		assert(SQLITE_OK == sqlite3_bind_int64(stmt, PN_MTIME, 0LL));
		assert(SQLITE_OK == sqlite3_bind_int64(stmt, PN_CTIME, 0LL));
	} else {
		assert(SQLITE_OK == sqlite3_bind_int64(stmt, PN_ATIME, sb.st_atime));
		assert(SQLITE_OK == sqlite3_bind_int64(stmt, PN_MTIME, sb.st_mtime));
		assert(SQLITE_OK == sqlite3_bind_int64(stmt, PN_CTIME, sb.st_ctime));
	}

	if (sqlite3_step(stmt) != SQLITE_DONE) {
		warnx("Could not add root node: %s", sqlite3_errmsg(db));
		closedir(current.dir);
		exit(EXIT_FAILURE);
	}

	current.row = sqlite3_last_insert_rowid(db);
	current.pos = strlen(argv[AR_PATH]);

	int sp = 0;
	char path[PATH_MAX];

	memcpy(path, argv[AR_PATH], current.pos);

	if (path[current.pos - 1] != '/') {
		path[current.pos] = '/';
		++current.pos;
	}

	path[current.pos] = 0;

	for (;;) {
		size_t len;
		unsigned char md5[16], sha1[20], sha256[32], sha512[64]; // because of SQLITE_STATIC

		errno = 0;

		struct dirent *ent = readdir(current.dir);

		if (ent) {
			switch (ent->d_type) {
				case DT_DIR:
					// create subnode and keep the parent primary key

					if (!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, "..")) {
						break;
					}

					printf("D %s%s\n", path, ent->d_name);

					if (sqlite3_reset(stmt) != SQLITE_OK) {
						warnx("Could not reset statement: %s", sqlite3_errmsg(db));
						break;
					}

					len = strlen(ent->d_name);

					assert(SQLITE_OK == sqlite3_bind_int64(stmt, PN_PARENT, current.row));
					assert(SQLITE_OK == sqlite3_bind_int(stmt, PN_TYPE, ent->d_type));
					assert(SQLITE_OK == sqlite3_bind_text(stmt, PN_NAME, ent->d_name, len, SQLITE_STATIC));
					//sqlite3_bind_null(stmt, PN_SIZE);
					assert(SQLITE_OK == sqlite3_bind_int64(stmt, PN_SIZE, 0LL));
					assert(SQLITE_OK == sqlite3_bind_null(stmt, PN_MD5));
					assert(SQLITE_OK == sqlite3_bind_null(stmt, PN_SHA1));
					assert(SQLITE_OK == sqlite3_bind_null(stmt, PN_SHA256));
					assert(SQLITE_OK == sqlite3_bind_null(stmt, PN_SHA512));

					memcpy(&(path[current.pos]), ent->d_name, len);
					stack[sp] = current;
					current.pos += len;
					path[current.pos] = 0;

					if (lstat(path, &sb)) { // like stat, except does not resolve symlinks
						warn("Could not stat directory: %s", path);
						//sqlite3_bind_null(stmt, PN_SIZE);
						assert(SQLITE_OK == sqlite3_bind_int64(stmt, PN_ATIME, 0LL));
						assert(SQLITE_OK == sqlite3_bind_int64(stmt, PN_MTIME, 0LL));
						assert(SQLITE_OK == sqlite3_bind_int64(stmt, PN_CTIME, 0LL));
					} else {
						assert(SQLITE_OK == sqlite3_bind_int64(stmt, PN_ATIME, sb.st_atime));
						assert(SQLITE_OK == sqlite3_bind_int64(stmt, PN_MTIME, sb.st_mtime));
						assert(SQLITE_OK == sqlite3_bind_int64(stmt, PN_CTIME, sb.st_ctime));
					}

					current.dir = opendir(path);

					assert(SQLITE_OK == sqlite3_bind_int(stmt, PN_ERROR, errno));

					status = sqlite3_step(stmt);

					if (status != SQLITE_DONE) {
						warnx("Could not execute statement: %s", sqlite3_errmsg(db));
					}

					if (!current.dir) {
						warn("Could not open directory: %s", path);
						current = stack[sp];
					} else if (status == SQLITE_DONE) {
						current.row = sqlite3_last_insert_rowid(db);
						path[current.pos] = '/';
						++current.pos;
						++sp;
					} else {
						closedir(current.dir);
						current = stack[sp];
					}

					path[current.pos] = 0;
					break;

				case DT_REG:
					printf("F %s%s\n", path, ent->d_name);

					if (sqlite3_reset(stmt) != SQLITE_OK) {
						warnx("Could not reset statement: %s", sqlite3_errmsg(db));
						break;
					}

					len = strlen(ent->d_name);

					assert(SQLITE_OK == sqlite3_bind_int64(stmt, PN_PARENT, current.row));
					assert(SQLITE_OK == sqlite3_bind_int(stmt, PN_TYPE, ent->d_type));
					assert(SQLITE_OK == sqlite3_bind_text(stmt, PN_NAME, ent->d_name, len, SQLITE_STATIC));

					memcpy(&(path[current.pos]), ent->d_name, len);
					path[current.pos + len] = 0;

					if (lstat(path, &sb)) {
						warn("Could not stat file: %s", path);
						//sqlite3_bind_null(stmt, PN_SIZE);
						assert(SQLITE_OK == sqlite3_bind_int64(stmt, PN_SIZE, 0LL));
						assert(SQLITE_OK == sqlite3_bind_int64(stmt, PN_ATIME, 0LL));
						assert(SQLITE_OK == sqlite3_bind_int64(stmt, PN_MTIME, 0LL));
						assert(SQLITE_OK == sqlite3_bind_int64(stmt, PN_CTIME, 0LL));
					} else {
						assert(SQLITE_OK == sqlite3_bind_int64(stmt, PN_SIZE, sb.st_size));
						assert(SQLITE_OK == sqlite3_bind_int64(stmt, PN_ATIME, sb.st_atime));
						assert(SQLITE_OK == sqlite3_bind_int64(stmt, PN_MTIME, sb.st_mtime));
						assert(SQLITE_OK == sqlite3_bind_int64(stmt, PN_CTIME, sb.st_ctime));
					}

					int fd = open(path, O_RDONLY);

					if (fd < 0) {
						warn("Could not open file descriptor: %s", path);

						assert(SQLITE_OK == sqlite3_bind_null(stmt, PN_MD5));
						assert(SQLITE_OK == sqlite3_bind_null(stmt, PN_SHA1));
						assert(SQLITE_OK == sqlite3_bind_null(stmt, PN_SHA256));
						assert(SQLITE_OK == sqlite3_bind_null(stmt, PN_SHA512));
					} else {
						MD5_CTX md5c;
						MD5_Init(&md5c);

						SHA_CTX sha1c;
						SHA1_Init(&sha1c);

						SHA256_CTX sha256c;
						SHA256_Init(&sha256c);

						SHA512_CTX sha512c;
						SHA512_Init(&sha512c);

						char buffer[BUFFER_SIZE];

						for (;;) {
							ssize_t n = read(fd, buffer, BUFFER_SIZE);

							if (!n) {
								MD5_Final(md5, &md5c);
								SHA1_Final(sha1, &sha1c);
								SHA256_Final(sha256, &sha256c);
								SHA512_Final(sha512, &sha512c);

								assert(SQLITE_OK == sqlite3_bind_blob(stmt, PN_MD5, md5, sizeof(md5), SQLITE_STATIC));
								assert(SQLITE_OK == sqlite3_bind_blob(stmt, PN_SHA1, sha1, sizeof(sha1), SQLITE_STATIC));
								assert(SQLITE_OK == sqlite3_bind_blob(stmt, PN_SHA256, sha256, sizeof(sha256), SQLITE_STATIC));
								assert(SQLITE_OK == sqlite3_bind_blob(stmt, PN_SHA512, sha512, sizeof(sha512), SQLITE_STATIC));

								break;
							} else if (n < 0) {
								warn("An error occured while reading the file: %s", path);

								assert(SQLITE_OK == sqlite3_bind_null(stmt, PN_MD5));
								assert(SQLITE_OK == sqlite3_bind_null(stmt, PN_SHA1));
								assert(SQLITE_OK == sqlite3_bind_null(stmt, PN_SHA256));
								assert(SQLITE_OK == sqlite3_bind_null(stmt, PN_SHA512));

								break;
							}

							MD5_Update(&md5c, buffer, n);
							SHA1_Update(&sha1c, buffer, n);
							SHA256_Update(&sha256c, buffer, n);
							SHA512_Update(&sha512c, buffer, n);
						}

						close(fd);
					}

					path[current.pos] = 0;

					assert(SQLITE_OK == sqlite3_bind_int(stmt, PN_ERROR, errno));

					if (sqlite3_step(stmt) != SQLITE_DONE) {
						warnx("Could not execute statement: %s", sqlite3_errmsg(db));
					}

					break;

				default:
					printf("%d %s%s\n", ent->d_type, path, ent->d_name);

					if (sqlite3_reset(stmt) != SQLITE_OK) {
						warnx("Could not reset statement: %s", sqlite3_errmsg(db));
						break;
					}

					assert(SQLITE_OK == sqlite3_bind_int64(stmt, PN_PARENT, current.row));
					assert(SQLITE_OK == sqlite3_bind_int(stmt, PN_TYPE, ent->d_type));
					assert(SQLITE_OK == sqlite3_bind_text(stmt, PN_NAME, ent->d_name, -1, SQLITE_STATIC));
					//sqlite3_bind_null(stmt, PN_SIZE);
					//sqlite3_bind_null(stmt, PN_ERROR);
					assert(SQLITE_OK == sqlite3_bind_int64(stmt, PN_SIZE, 0LL));
					assert(SQLITE_OK == sqlite3_bind_int(stmt, PN_ERROR, 0));
					assert(SQLITE_OK == sqlite3_bind_null(stmt, PN_MD5));
					assert(SQLITE_OK == sqlite3_bind_null(stmt, PN_SHA1));
					assert(SQLITE_OK == sqlite3_bind_null(stmt, PN_SHA256));
					assert(SQLITE_OK == sqlite3_bind_null(stmt, PN_SHA512));

					if (sqlite3_step(stmt) != SQLITE_DONE) {
						warnx("Could not execute statement: %s", sqlite3_errmsg(db));
					}
			}
		} else {
			if (errno) {
				warn("An error occured during readdir within %s", path);
			}

			closedir(current.dir);

			if (!sp) {
				break;
			}

			--sp;
			current = stack[sp];
			path[current.pos] = 0;
		}
	}

	if (transaction && sqlite3_exec(db, "COMMIT", NULL, NULL, &sqlerr) != SQLITE_OK) {
		warnx("Could not commit transaction: %s", sqlerr);
		sqlite3_free(sqlerr);
		exit(EXIT_FAILURE);
	}

	exit(EXIT_SUCCESS);
}
