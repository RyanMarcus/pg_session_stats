// < begin copyright > 
// Copyright Ryan Marcus 2019
// 
// This file is part of pg_session_stats.
// 
// pg_session_stats is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
// 
// pg_session_stats is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
// 
// You should have received a copy of the GNU General Public License
// along with pg_session_stats.  If not, see <http://www.gnu.org/licenses/>.
// 
// < end copyright > 
#include <sys/types.h>
#include <unistd.h>
#include <sqlite3.h>
#include <time.h>
#include <pwd.h>

#include "postgres.h"
#include "fmgr.h"
#include "executor/executor.h"
#include "utils/guc.h"

typedef struct PerfInfo {
  pid_t pid;
  size_t num_executors;
  uint64_t total_cpu;
} PerfInfo;


PG_MODULE_MAGIC;
void _PG_init(void);
void _PG_fini(void);
void saveInfo(void);

static void pss_ExecutorStart(QueryDesc *queryDesc, int eflags);
static void pss_ExecutorEnd(QueryDesc *queryDesc);
static ExecutorStart_hook_type prev_ExecutorStart = NULL;
static ExecutorEnd_hook_type prev_ExecutorEnd = NULL;

static char* pg_session_stats_path;

const char* pg_session_stats_insert_sql = "INSERT INTO log VALUES(?, ?, ?, ?, ?);";

pid_t read_pg_pid_desc(void);
void trimwhitespace(char *str);
void read_file(const char* fp, char* buf, size_t len);

PerfInfo GLOBAL_TABLE[4096]; // maximum of 4096 parallel queries

void trimwhitespace(char *str) {
  size_t length;

  length = strlen(str);

  if (length == 0) return;
  length--;
  
  while (length > 0 && isspace(str[length]))
    str[length--] = '\0';
  
}

pid_t read_pg_pid_desc() {
  char path[4096];
  char desc[4096];
  pid_t pid;
  FILE* f;
  size_t length, curr;
  
  memset(path, 0, 4096);
  pid = getpid();
  snprintf(path, 4096, "/proc/%d/cmdline", pid);

  f = fopen(path, "r");
  if (!f) {
    elog(ERROR, "could not read proc description for PID %d", pid);
    return 0;
  }

  memset(desc, 0, 4096);
  
  if (fread(desc, sizeof(char), 4096, f) == 0) {
    elog(ERROR, "could not fread from proc description for PID %d", pid);
    return 0;
  }
  trimwhitespace(desc);

  // example: postgres: parallel worker for PID 14810
  // if the last character is not a numeral, this is the main executor.
  length = strlen(desc);
  if (!isdigit(desc[length - 1])) return pid;

  // otherwise, walk backwards through the string until we hit a non-numeral
  curr = length - 1;
  while (curr > 0 && isdigit(desc[curr])) curr--;

  return (pid_t) atoi(desc + curr);
}

void read_file(const char* fp, char* buf, size_t len) {
  FILE* f;

  memset(buf, 0, len);

  f = fopen(fp, "r");
  if (!f) {
    elog(ERROR, "could not open file");
    return;
  }

  if (fread(buf, sizeof(char), len, f) == 0) {
    elog(ERROR, "could not fread from file status");
    return;
  }
}

void _PG_init(void) {
  char defaultPath[4096];
  const char *homedir;
  
  if ((homedir = getenv("HOME")) == NULL) {
    homedir = getpwuid(getuid())->pw_dir;
  }


  snprintf(defaultPath, 4096, "%s/pgss.sqlite", homedir);
  
  prev_ExecutorStart = ExecutorStart_hook;
  ExecutorStart_hook = pss_ExecutorStart;

  prev_ExecutorEnd = ExecutorEnd_hook;
  ExecutorEnd_hook = pss_ExecutorEnd;

  DefineCustomStringVariable("pg_session_stats.path",
                             "path where a SQLite DB can be created",
                             "File path to where a SQLite DB can be created to store intermediate results",
                             &pg_session_stats_path,
                             defaultPath,
                             PGC_POSTMASTER,
                             0, NULL, NULL, NULL);
  elog(LOG, "Using %s as DB path", defaultPath);

}

void _PG_fini(void) {
  elog(LOG, "finished extension");
}


static void pss_ExecutorStart(QueryDesc *queryDesc, int eflags) {
  /*
  pid_t pid, parent;
  
  pid = getpid();
  parent = read_pg_pid_desc();
  elog(LOG, "My (PID: %d) parent executor PID is: %d", pid, parent);
  */
  if (prev_ExecutorStart) {
    prev_ExecutorStart(queryDesc, eflags);
  } else {
    standard_ExecutorStart(queryDesc, eflags);
  }
}

void saveInfo() {
  sqlite3* db;
  sqlite3_stmt* res;
  int rc;
  pid_t pid, parent;
  char buf[8096];
  char buf2[8096];
  
  clock_t usage = clock();
  double asSeconds = (double)usage / (double)CLOCKS_PER_SEC;

  memset(buf, 0, 8096);
  memset(buf2, 0, 8096);
  
  pid = getpid();
  parent = read_pg_pid_desc();
  read_file("/proc/self/status", buf, 8096);
  read_file("/proc/self/io", buf2, 8096);
  
  rc = sqlite3_open(pg_session_stats_path, &db);
  sqlite3_busy_timeout(db, 5000);

  if (rc != SQLITE_OK) {
    elog(ERROR, "Could not open SQLite database for logging: %s",
         sqlite3_errmsg(db));
    return;
  }

  rc = sqlite3_exec(db,
                    "CREATE TABLE IF NOT EXISTS log ("
                    "  master_pid INT,"
                    "  my_pid     INT,"
                    "  usage      REAL,"
                    "  procstatus TEXT,"
                    "  procio     TEXT"
                    ");",
                    NULL, NULL, NULL);

  if (rc != SQLITE_OK) {
    elog(ERROR, "Could not ensure schema was created: %s",
         sqlite3_errmsg(db));
    return;
  }

  rc = sqlite3_prepare_v2(db, pg_session_stats_insert_sql, -1, &res, NULL);

  if (rc != SQLITE_OK) {
    elog(ERROR, "Could not prepare insert statement: %s",
         sqlite3_errmsg(db));
    return;
  }

  sqlite3_bind_int(res, 1, parent);
  sqlite3_bind_int(res, 2, pid);
  sqlite3_bind_double(res, 3, asSeconds);
  sqlite3_bind_text(res, 4, buf, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(res, 5, buf2, -1, SQLITE_TRANSIENT);

  if (sqlite3_step(res) != SQLITE_DONE) {
    elog(ERROR, "Could not insert data: %s",
         sqlite3_errmsg(db));
    return;
  }

  sqlite3_finalize(res);
  sqlite3_close(db);
}

static void pss_ExecutorEnd(QueryDesc *queryDesc) {
  pid_t pid, parent;
  
  pid = getpid();
  parent = read_pg_pid_desc();
  elog(LOG, "My (PID: %d) parent executor PID is: %d", pid, parent);

  saveInfo();
  
  if (prev_ExecutorEnd) {
    prev_ExecutorEnd(queryDesc);
  } else {
    standard_ExecutorEnd(queryDesc);
  }
}

