#include <err.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include "sqlite3.h"

#define AR_DATABASE 1
#define AR_STMT 2
#define AR_COUNT 2

#define BLOCK_SIZE 256

sqlite3 *db;
sqlite3_stmt *stmt;
int firstrow = 1;

static void escape(unsigned const char *str)
{
	if (!str) {
		return;
	}

	unsigned const char *p = str;

	while (*p) {
		if (*p < 32 || *p == '"' || *p == ',') {
			putc('"', stdout);
			p = str;
			while (*p) {
				if (*p == '"') {
					putc('"', stdout);
				}
				putc(*p, stdout);
				++p;
			}
			putc('"', stdout);
			return;
		}

		++p;
	}

	fputs((const char*) str, stdout);
}

static void putcsv(int argc, char *argv[])
{
	escape((unsigned const char*) argv[0]);

	int i;

	for (i = 1; i < argc; ++i) {
		putc(',', stdout);
		escape((unsigned const char*) argv[i]);
	}

	putc('\n', stdout);
}

static int callback(void *unused, int argc, char *argv[], char *name[])
{
	if (!argc) {
		errx(EXIT_FAILURE, "no arguments provided to callback");
	}

	if (firstrow) {
		putcsv(argc, name);
		firstrow = 0;
	}

	putcsv(argc, argv);

	return 0;
}

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
	if (argc < AR_COUNT) {
		exit(EXIT_SUCCESS);
	}

	int status = sqlite3_open(argv[AR_DATABASE], &db);
	char *sqlerr;

	atexit(cleanup_db);

	if (status != SQLITE_OK) {
		errx(EXIT_FAILURE, "Could not open database: %s", sqlite3_errmsg(db));
	}

	{
		char *s = getenv("SQLITE_DBCONFIG_ENABLE_FKEY");
		int fk = !s || atoi(s);

		if (sqlite3_db_config(db, SQLITE_DBCONFIG_ENABLE_FKEY, fk, &status) != SQLITE_OK) {
			errx(EXIT_FAILURE, "Could not set SQLITE_DBCONFIG_ENABLE_FKEY to %s: %s", s, sqlite3_errmsg(db));
		}

		if (fk != status) {
			errx(EXIT_FAILURE, "Could not set SQLITE_DBCONFIG_ENABLE_FKEY to %s: status = %d", s, status);
		}
	}

	if (sqlite3_exec(db, "PRAGMA recursive_triggers = ON", NULL, NULL, &sqlerr) != SQLITE_OK) {
		errx(EXIT_FAILURE, "Could not enable recursive triggers: %s", sqlerr);
	}

	if (argc > AR_COUNT) {
		if (sqlite3_prepare(db, argv[AR_STMT], -1, &stmt, NULL) != SQLITE_OK) {
			errx(EXIT_FAILURE, "Could not prepare statement: %s", sqlite3_errmsg(db));
		}

		atexit(cleanup_stmt);

		int i;
		int n = sqlite3_column_count(stmt);

		if (n) {
			escape((unsigned const char*) sqlite3_column_name(stmt, 0));

			for (i = 1; i < n; ++i) {
				putc(',', stdout);
				escape((unsigned const char*) sqlite3_column_name(stmt, i));
			}

			putc('\n', stdout);
		}

		for (i = 3; i < argc; ++i) {
			sqlite3_bind_text(stmt, i - 2, argv[i], -1, SQLITE_STATIC);
		}

		for (;;) {
			status = sqlite3_step(stmt);

			switch (status) {
				case SQLITE_ROW:
					escape(sqlite3_column_text(stmt, 0));

					for (i = 1; i < n; ++i) {
						putc(',', stdout);
						escape(sqlite3_column_text(stmt, i));
					}

					putc('\n', stdout);

					break;

				case SQLITE_DONE:
					exit(EXIT_SUCCESS);

				default:
					errx(EXIT_FAILURE, "sqlite3_step returned %d: %s", status, sqlite3_errmsg(db));
			}
		}
	} else {
		size_t size = 0, i;
		ssize_t n;
		char *sql = NULL;
		char *p;

		do {
			n = size + BLOCK_SIZE;
			p = realloc(sql, n);

			if (!p) {
				free(sql);
				err(EXIT_FAILURE, "An error occured during realloc");
			}

			sql = p;
			p += size;
			size = n;
			i = 0;

			do {
				n = read(STDIN_FILENO, p + i, BLOCK_SIZE - i);
				if (!n) {
					break;
				}
				if (n < 0) {
					err(EXIT_FAILURE, "An error occured while reading from standard input");
				}
				i += n;
			} while (i < BLOCK_SIZE);
		} while (n);

		if (i >= BLOCK_SIZE) {
			i = size;
			++size;
			p = realloc(sql, size);

			if (!p) {
				free(sql);
				err(EXIT_FAILURE, "An error occured during final realloc");
			}
		}

		p[i] = 0;

		status = sqlite3_exec(db, sql, callback, NULL, &sqlerr);

		free(sql);

		if (status != SQLITE_OK) {
			errx(EXIT_FAILURE, "sqlite3_exec returned %d: %s", status, sqlerr);
		}
	}

	exit(EXIT_SUCCESS);
}
