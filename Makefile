EXTENSION = pg_session_stats
MODULE_big = pg_session_stats
DATA = pg_session_stats--0.0.1.sql
OBJS = main.o 
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
SHLIB_LINK = -lsqlite3
include $(PGXS)
