#include <string.h>
#include <stdlib.h>
#include <curses.h>
#include <term.h>

#include "dtach.h"

enum
{
	MATCH_DELETE = 0,
	MATCH_SMKX = 1,
	MATCH_RMKX = 2,
	MATCH_SMCUP = 3,
	MATCH_RMCUP = 4,
};

struct needle
{
	struct needle *next;
	char *str;
	short *table;
	short j;
	unsigned short action;
};

/*
** Set up common answerback needles. Since none has any KMP fallback matches, we
** can use the same table for all of them.
*/
static short anstables[] = {-1, 0, 0, 0, 0};
static struct needle ans4 = {
	.next = NULL,
	.str = "\033[>0c",
	.table = anstables,
	.j = 0,
	.action = MATCH_DELETE,
};
static struct needle ans3 = {
	.next = &ans4,
	.str = "\033[>c",
	.table = anstables,
	.j = 0,
	.action = MATCH_DELETE,
};
static struct needle ans2 = {
	.next = &ans3,
	.str = "\033[c",
	.table = anstables,
	.j = 0,
	.action = MATCH_DELETE,
};
static struct needle ans1 = {
	.next = &ans2,
	.str = "\033Z",
	.table = anstables,
	.j = 0,
	.action = MATCH_DELETE,
};
static struct needle ans0 = {
	.next = &ans1,
	.str = "\005",
	.table = anstables,
	.j = 0,
	.action = MATCH_DELETE,
};
static struct needle smkx = {
	.action = MATCH_SMKX,
};
static struct needle rmkx = {
	.action = MATCH_RMKX,
};
static struct needle smcup = {
	.action = MATCH_SMCUP,
};
static struct needle rmcup = {
	.action = MATCH_RMCUP,
};

static struct needle *pincushion = &ans0, *pctail = &ans4;

/*
** Sets up the KMP fallback table for a needle.  Note that the table must be
** allocated already.
*/
static void kmp_setup(struct needle *n)
{
	short i, j;

	for (i = 0, j = -2; n->str[i]; i++)
	{
		while (j >= 0 && n->str[i - 1] != n->str[j])
			j = n->table[j];
		n->table[i] = ++j;
	}
}

/*
** Initializes the parser.
*/
int parser_init(struct pty *p)
{
	p->leftover = 0;
	p->smkx = 0;
	if (setupterm(NULL, p->fd, NULL) == ERR)
		return 1;

	smkx.str = tigetstr("smkx");
	if (smkx.str)
	{
		smkx.table = malloc(strlen(smkx.str) * sizeof (int));
		if (!smkx.table)
			return 1;
		kmp_setup(&smkx);
		pctail->next = &smkx;
		pctail = &smkx;
	}
	rmkx.str = tigetstr("rmkx");
	if (rmkx.str)
	{
		rmkx.table = malloc(strlen(rmkx.str) * sizeof (int));
		if (!rmkx.table)
			return 1;
		kmp_setup(&rmkx);
		pctail->next = &rmkx;
		pctail = &rmkx;
	}
	smcup.str = tigetstr("smcup");
	if (smcup.str)
	{
		smcup.table = malloc(strlen(smcup.str) * sizeof (int));
		if (!smcup.table)
			return 1;
		kmp_setup(&smcup);
		pctail->next = &smcup;
		pctail = &smcup;
	}
	rmcup.str = tigetstr("rmcup");
	if (rmcup.str)
	{
		rmcup.table = malloc(strlen(rmcup.str) * sizeof (int));
		if (!rmcup.table)
			return 1;
		kmp_setup(&rmcup);
		pctail->next = &rmcup;
		pctail = &rmcup;
	}
	return 0;
}

/*
** When successful, returns first needle found in haystack and places the index
** right after the needle in next.  Otherwise, returns NULL and places count in
** next.
**
** NOTE: This function is not generic in that it returns immediately when a
** match is found.  This leaves the pincushion's j-values in an inconsistent
** state, since the remaining needles for that pass have not been processed.  We
** clear the j-values on a match, so this doesn't hurt here.
*/
static struct needle *kmp_search(const char *haystack, unsigned int count,
		int *next)
{
	unsigned int i;
	struct needle *n;

	for (i = 0; i < count; i++)
	{
		for (n = pincushion; n; n = n->next)
		{
			while (n->j >= 0 && haystack[i] != n->str[n->j])
				n->j = n->table[n->j];
			if (!n->str[++n->j])
			{
				/* Found a match */
				*next = i + 1;
				return n;
			}
		}
	}
	/* Reached end with no match (i == count) */
	*next = count;
	return NULL;
}

/*
** Process as much of the buffer as possible, rewriting it in place.  Returns
** the new length of the buffer.  Puts the number of leftover bytes in rem.
*/
int parse_buf(struct pty *p, unsigned int count, unsigned int *rem)
{
	/* where to put good characters */
	char *end;
	/* next character being examined */
	char *next;
	struct needle *n;
	int i, c;

	end = next = p->buf + p->leftover;
	while ((n = kmp_search(next, count, &i)))
	{
		/* c is number of "good" characters to append to buffer */
		c = i;
		switch (n->action)
		{
			case MATCH_DELETE:
				c -= n->j;
				break;
			case MATCH_SMKX:
				p->smkx = 1;
				break;
			case MATCH_RMKX:
				p->smkx = 0;
				break;
			case MATCH_SMCUP:
				p->smcup = 1;
				break;
			case MATCH_RMCUP:
				p->smcup = 0;
				break;
		}
		if (c > 0)
			memmove(end, next, c);
		end += c;
		count -= i;
		next += i;
		for (n = pincushion; n; n = n->next)
			n->j = 0;
	}
	/* Here c is number of potentially "bad" characters at end of buffer */
	c = 0;
	for (n = pincushion; n; n = n->next)
		if (n->j > c)
			c = n->j;
	/* When kmp_search fails, i is set to count */
	i -= c;
	if (i > 0)
		memmove(end, next, i);
	end += i;
	*rem = c;
	return end - p->buf;
}

/*
** Restores the state of a pty by replaying control sequences to the given fd.
*/
int restore_state(struct pty *p, int fd)
{
	int len, written, n;

	if (p->smcup)
	{
		len = strlen(smcup.str);
		n = 0;
		for (written = 0; written < len; written += n)
		{
			n = write(fd, smcup.str + written, len - written);
			if (n <= 0)
			{
				if (n == 0 || errno == EAGAIN)
					break;
				if (errno != EINTR)
					return errno;
				n = 0;
			}
		}
	}
	if (p->smkx)
	{
		len = strlen(smkx.str);
		n = 0;
		for (written = 0; written < len; written += n)
		{
			n = write(fd, smkx.str + written, len - written);
			if (n <= 0)
			{
				if (n == 0 || errno == EAGAIN)
					break;
				if (errno != EINTR)
					return errno;
				n = 0;
			}
		}
	}
	return 0;
}
