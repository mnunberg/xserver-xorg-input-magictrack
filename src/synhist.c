#include "synhist.h"
#include "yolog.h"
#include <stdlib.h>

YOLOG_STATIC_INIT("synhist.c", YOLOG_DEBUG);

#define BACKLOG(l, a) \
	(l->idx - (a) + SYNHIST_LOG_MAX) % SYNHIST_LOG_MAX

void synhist_reset(SynhistLog *log)
{
	log->idx = 0;
	log->count = 0;
}

#define _synhist_last(l, bk, vp, t) \
	if(l->count < bk) { \
		yolog_err("Not enough inside buffer");\
		*vp = NULL; \
	} else { \
		int i, idx; \
		for(i = 0; i < bk; i++) { \
			idx = BACKLOG(l, i+1); \
			(*vp)[i] = l->records[idx].t; \
		} \
	} \

void synhist_last_values(SynhistLog *log, int backlog, int **vals)
{
	_synhist_last(log, backlog, vals, value);
}

void synhist_last_times(SynhistLog *log, int backlog, int **vals)
{
	_synhist_last(log, backlog, vals, millis);
}

void synhist_set(SynhistLog *log, int value, int millis)
{

	log->records[log->idx].value = value;
	log->records[log->idx].millis = millis;
	log->idx++;
	log->idx %= (SYNHIST_LOG_MAX);
	log->count++;

}


