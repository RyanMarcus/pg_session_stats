# pg_session_stats

This extension lets you track PostgreSQL resource usage at the level of a session (connection). Notably, this library properly tracks usage across parallel workers. To instrument a single query, one should create a new session, execute that query, then end the session.

To install, ensure you have `sqlite3` installed and run:

```bash
make USE_PGXS=1 install
```

The extension makes a few assumptions that might not stay true, but are true in PG12:

* That `ExecutorEnd` hooks are reliably called after all of what could be considered query processing has completed.
* That the `/proc/{PID}/cmdline` of worker processes will end with the main executor's PID (if anyone knows a better way to connect worker PIDs to the primary executor, let me know).

Since dealing with PostgreSQL's shared memory and locking system is not my idea of a good time, this extention saves the results into a SQLite3 database. You can set the location of this database by setting `pg_session_stats.path` in your `postgresql.conf`.

The created DB has a single table called `log` with the following schema:

```sql
CREATE TABLE log (
    master_pid INT,
    my_pid     INT, 
    usage      REAL,
    procstatus TEXT
);
```


* The `master_pid` column will contain the PID of the main executor process (i.e., the one given by `select pg_backend_pid();`). 
* The `my_pid` column contains the PID of executor (which may be equal to `master_pid`). 
* The `usage` column contains the total amount of *CPU time* used by that executor.
* The `procstatus` column contains the output of `/proc/{PID}/status` for that executor after query processing was complete.
