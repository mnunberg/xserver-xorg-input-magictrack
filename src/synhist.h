#ifndef SYNHIST_H
#define SYNHIST_H

#include <stdint.h>

#define SYNHIST_LOG_MAX 10
#define SYNHIST_OOB INT32_MAX;
#define SYNHIST_DATA_MAX 4

#define SYNHIST_IDX_AVG 3
/*
 * Each finger has X and Y coordinates.
 *
 */

typedef struct {
	int value;
	int millis;
} SynhistRec;

typedef struct {
	int idx;
	int count;
	SynhistRec records[SYNHIST_LOG_MAX];
} SynhistLog;

void synhist_reset(SynhistLog *log);
void synhist_last_values(SynhistLog *log, int backlog, int **valp);
void synhist_last_times(SynhistLog *log, int backlog, int **vals);
void synhist_set(SynhistLog *log, int value, int millis);

#endif /*SYNHIST_H*/
