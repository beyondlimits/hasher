#include <err.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include "sqlite3.h"

#define BLOCK_SIZE 256

sqlite3 *db;        // database handle
sqlite3_stmt *stmt; // sql statement
int firstrow = 1;

static void escape(str)
char *str;
{
	if (!str) {
		return;
	}

	unsigned char *p = str;

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

	fputs(str, stdout);
}

static void putcsv(argc, argv)
int argc;
char **argv;
{
	escape(argv[0]);

	int i;

	for (i = 1; i < argc; ++i) {
		putc(',', stdout);
		escape(argv[i]);
	}

	putc('\n', stdout);
}

static int callback(unused, argc, argv, name)
void *unused;
int argc;
char **argv;
char **name;
{
	if (!argc) {
		fputs("argc was zero.\n", stderr);
		exit(EXIT_FAILURE);
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
	sqlite3_close(db);
}

static void cleanup_stmt()
{
	sqlite3_finalize(stmt);
}

int main(argc, argv)
int argc;
char **argv;
{
	if (argc < 2) {
		exit(EXIT_SUCCESS);
	}

	int status = sqlite3_open(argv[1], &db);
	char *sqlerr;

	atexit(cleanup_db);

	if (status != SQLITE_OK) {
		fprintf(stderr, "Could not open database: %s\n", sqlite3_errmsg(db));
		exit(EXIT_FAILURE);
	}

	if (sqlite3_db_config(db, SQLITE_DBCONFIG_ENABLE_FKEY, 1, &status) != SQLITE_OK) {
		fprintf(stderr, "Could not enforce foreign keys: %s\n", sqlite3_errmsg(db));
		exit(EXIT_FAILURE);
	}

	if (!status) {
		fputs("Could not enforce foreign keys.\n", stderr);
		exit(EXIT_FAILURE);
	}

	if (sqlite3_exec(db, "PRAGMA recursive_triggers = ON", NULL, NULL, &sqlerr) != SQLITE_OK) {
		fprintf(stderr, "Could not enable recursive triggers: %s\n", sqlerr);
		sqlite3_free(sqlerr);
		exit(EXIT_FAILURE);
	}

	if (argc > 2) {
		if (sqlite3_prepare(db, argv[2], -1, &stmt, NULL) != SQLITE_OK) {
			fprintf(stderr, "Could not prepare statement: %s\n", sqlite3_errmsg(db));
			exit(EXIT_FAILURE);
		}

		atexit(cleanup_stmt);

		int i;
		int n = sqlite3_column_count(stmt);

		if (n) {
			escape(sqlite3_column_name(stmt, 0));

			for (i = 1; i < n; ++i) {
				putc(',', stdout);
				escape(sqlite3_column_name(stmt, i));
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
					fprintf(stderr, "sqlite3_step returned %d: %s\n", status, sqlite3_errmsg(db));
					exit(EXIT_FAILURE);
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
			fprintf(stderr, "sqlite3_exec returned %d: %s\n", status, sqlerr);
			sqlite3_free(sqlerr);
			exit(EXIT_FAILURE);
		}
	}

	exit(EXIT_SUCCESS);
}
