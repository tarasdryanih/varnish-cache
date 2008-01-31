/*-
 * Copyright (c) 2006 Verdens Gang AS
 * Copyright (c) 2006-2008 Linpro AS
 * All rights reserved.
 *
 * Author: Anders Berg <andersb@vgnett.no>
 * Author: Poul-Henning Kamp <phk@phk.freebsd.dk>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * $Id$
 *
 * Obtain log data from the shared memory log, order it by session ID, and
 * display it in Apache / NCSA combined log format:
 *
 *	%h %l %u %t "%r" %>s %b "%{Referer}i" "%{User-agent}i"
 *
 * where the fields are defined as follows:
 *
 *	%h		Client host name or IP address (always the latter)
 *	%l		Client user ID as reported by identd (always "-")
 *	%u		User ID if using HTTP authentication, or "-"
 *	%t		Date and time of request
 *	%r		Request line
 *	%s		Status code
 *	%b		Length of reply body, or "-"
 *	%{Referer}i	Contents of "Referer" request header
 *	%{User-agent}i	Contents of "User-agent" request header
 *
 * Actually, we cheat a little and replace "%r" with something close to
 * "%m http://%{Host}i%U%q %H", where the additional fields are:
 *
 *	%m		Request method
 *	%{Host}i	Contents of "Host" request header
 *	%U		URL path
 *	%q		Query string
 *	%H		Protocol version
 *
 * TODO:	- Log in any format one wants
 *		- Maybe rotate/compress log
 */

#include <ctype.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <limits.h>

#ifndef HAVE_DAEMON
#include "compat/daemon.h"
#endif

#include "vsb.h"
#include "vpf.h"

#include "libvarnish.h"
#include "shmlog.h"
#include "varnishapi.h"

static volatile sig_atomic_t reopen;

static struct logline {
	char *df_H;			/* %H, Protocol version */
	char *df_Host;			/* %{Host}i */
	char *df_Referer;		/* %{Referer}i */
	char *df_Uq;			/* %U%q, URL path and query string */
	char *df_User_agent;		/* %{User-agent}i */
	char *df_b;			/* %b, Bytes */
	char *df_h;			/* %h (host name / IP adress)*/
	char *df_m;			/* %m, Request method*/
	char *df_s;			/* %s, Status */
	char *df_u;			/* %u, Remote user */
	int bogus;			/* bogus request */
} **ll;

static size_t nll;

static time_t t;

static int
isprefix(const char *str, const char *prefix, const char *end, const char **next)
{

	while (str < end && *str && *prefix &&
	    tolower((int)*str) == tolower((int)*prefix))
		++str, ++prefix;
	if (*str && *str != ' ')
		return (0);
	if (next) {
		while (str < end && *str && *str == ' ')
			++str;
		*next = str;
	}
	return (1);
}

/*
 * Returns a copy of the first consecutive sequence of non-space
 * characters in the string.
 */
static char *
trimfield(const char *str, const char *end)
{
	size_t len;
	char *p;

	/* skip leading space */
	while (str < end && *str && *str == ' ')
		++str;

	/* seek to end of field */
	for (len = 0; &str[len] < end && str[len]; ++len)
		if (str[len] == ' ')
			break;

	/* copy and return */
	p = malloc(len + 1);
	assert(p != NULL);
	memcpy(p, str, len);
	p[len] = '\0';
	return (p);
}

/*
 * Returns a copy of the entire string with leading and trailing spaces
 * trimmed.
 */
static char *
trimline(const char *str, const char *end)
{
	size_t len;
	char *p;

	/* skip leading space */
	while (str < end && *str && *str == ' ')
		++str;

	/* seek to end of string */
	for (len = 0; &str[len] < end && str[len]; ++len)
		 /* nothing */ ;

	/* trim trailing space */
	while (len && str[len - 1] == ' ')
		--len;

	/* copy and return */
	p = malloc(len + 1);
	assert(p != NULL);
	memcpy(p, str, len);
	p[len] = '\0';
	return (p);
}

static int
h_ncsa(void *priv, enum shmlogtag tag, unsigned fd,
    unsigned len, unsigned spec, const char *ptr)
{
	const char *end, *next;
	char *q;
	FILE *fo;
	long l;
	struct tm tm;
	char tbuf[40];
	struct logline *lp;

	end = ptr + len;

	if (!(spec & VSL_S_CLIENT || spec & VSL_S_BACKEND))
		return (reopen);

	if (fd >= nll) {
		struct logline **newll = ll;
		size_t newnll = nll;

		while (fd >= newnll)
			newnll += newnll + 1;
		newll = realloc(newll, newnll * sizeof *newll);
		assert(newll != NULL);
		memset(newll + nll, 0, (newnll - nll) * sizeof *newll);
		ll = newll;
		nll = newnll;
	}
	if (ll[fd] == NULL) {
		ll[fd] = calloc(sizeof *ll[fd], 1);
		assert(ll[fd] != NULL);
	}
	lp = ll[fd];

	switch (tag) {
	case SLT_BackendOpen:
		if (!(spec & VSL_S_BACKEND))
			break;
		if (lp->df_h != NULL)
			lp->bogus = 1;
		else {
			if (isprefix(ptr, "default", end, &next))
				lp->df_h = trimfield(next, end);
			else
				lp->df_h = trimfield(ptr, end);
		}
		break;
	case SLT_ReqStart:
		if (lp->df_h != NULL)
			lp->bogus = 1;
		else
			lp->df_h = trimfield(ptr, end);
		break;

	case SLT_TxRequest:
		if (!(spec & VSL_S_BACKEND))
			break;
	case SLT_RxRequest:
		if (tag == SLT_RxRequest && (spec & VSL_S_BACKEND))
			break;

		if (lp->df_m != NULL)
			lp->bogus = 1;
		else
			lp->df_m = trimline(ptr, end);
		break;

	case SLT_TxURL:
		if (!(spec & VSL_S_BACKEND))
			break;
	case SLT_RxURL:
		if (tag == SLT_RxURL && (spec & VSL_S_BACKEND))
			break;

		if (lp->df_Uq != NULL)
			lp->bogus = 1;
		else
			lp->df_Uq = trimline(ptr, end);
		break;

	case SLT_TxProtocol:
		if (!(spec & VSL_S_BACKEND))
			break;
	case SLT_RxProtocol:
		if (tag == SLT_RxProtocol && (spec & VSL_S_BACKEND))
			break;

		if (lp->df_H != NULL)
			lp->bogus = 1;
		else
			lp->df_H = trimline(ptr, end);
		break;

	case SLT_RxStatus:
		if (!(spec & VSL_S_BACKEND))
			break;
	case SLT_TxStatus:
		if (tag == SLT_TxStatus && (spec & VSL_S_BACKEND))
			break;

		if (lp->df_s != NULL)
			lp->bogus = 1;
		else
			lp->df_s = trimline(ptr, end);
		break;

	case SLT_TxHeader:
		if (!(spec & VSL_S_BACKEND))
			break;
	case SLT_RxHeader:
		if (tag == SLT_RxHeader && (spec & VSL_S_BACKEND)) {
			if (isprefix(ptr, "content-length:", end, &next)) {
				lp->df_b = trimline(next, end);
			} else if (isprefix(ptr, "date:", end, &next)) {
				if (strptime(trimline(next, end), "%a, %d %b %Y %T", &tm))
					t = mktime(&tm);
			}
			break;
		}
		if (isprefix(ptr, "user-agent:", end, &next))
			lp->df_User_agent = trimline(next, end);
		else if (isprefix(ptr, "referer:", end, &next))
			lp->df_Referer = trimline(next, end);
		else if (isprefix(ptr, "authorization:", end, &next) &&
		    isprefix(next, "basic", end, &next))
			lp->df_u = trimline(next, end);
		else if (isprefix(ptr, "host:", end, &next))
			lp->df_Host = trimline(next, end);
		break;

	case SLT_Length:
		if (lp->df_b != NULL)
			lp->bogus = 1;
		else
			lp->df_b = trimline(ptr, end);
		break;

	default:
		break;
	}

	if ((spec & VSL_S_CLIENT) && tag != SLT_ReqEnd)
		return (reopen);

	if ((spec & VSL_S_BACKEND) && tag != SLT_BackendReuse &&
	    (tag != SLT_BackendClose || lp->df_Uq))
		return (reopen);

	if (tag == SLT_ReqEnd) {
		if (sscanf(ptr, "%*u %*u.%*u %ld.", &l) != 1)
			lp->bogus = 1;
		else
			t = l;
	}

	if (!lp->bogus) {
		fo = priv;

		/* %h */
		if (!lp->df_h && spec & VSL_S_BACKEND)
			fprintf(fo, "127.0.0.1 ");
		else
			fprintf(fo, "%s ", lp->df_h ? lp->df_h : "-");

		/* %l */
		fprintf(fo, "- ");

		/* %u: decode authorization string */
		if (lp->df_u != NULL) {
			char *rubuf;
			size_t rulen;

			base64_init();
			rulen = ((strlen(lp->df_u) + 3) * 4) / 3;
			rubuf = malloc(rulen);
			assert(rubuf != NULL);
			base64_decode(rubuf, rulen, lp->df_u);
			q = strchr(rubuf, ':');
			if (q != NULL)
				*q = '\0';
			fprintf(fo, "%s ", rubuf);
			free(rubuf);
		} else {
			fprintf(fo, "- ");
		}

		/* %t */
		localtime_r(&t, &tm);
		strftime(tbuf, sizeof tbuf, "[%d/%b/%Y:%T %z]", &tm);
		fprintf(fo, "%s ", tbuf);

		/*
		 * Fake "%r".  This would be a lot easier if Varnish
		 * normalized the request URL.
		 */
		fprintf(fo, "\"%s ", lp->df_m);
		if (lp->df_Host) {
			if (strncmp(lp->df_Host, "http://", 7) != 0)
				fprintf(fo, "http://");
			fprintf(fo, lp->df_Host);
		}
		fprintf(fo, "%s ", lp->df_Uq);
		fprintf(fo, "%s\" ", lp->df_H);

		/* %s */
		fprintf(fo, "%s ", lp->df_s);

		/* %b */
		fprintf(fo, "%s ", lp->df_b);

		/* %{Referer}i */
		fprintf(fo, "\"%s\" ",
		    lp->df_Referer ? lp->df_Referer : "-");

		/* %{User-agent}i */
		fprintf(fo, "\"%s\"\n",
		    lp->df_User_agent ? lp->df_User_agent : "-");

		/* hack: flush after every line if writing to file */
		if (fo != stdout)
			fflush(fo);
	}

	/* clean up */
#define freez(x) do { if (x) free(x); x = NULL; } while (0);
	freez(lp->df_H);
	freez(lp->df_Host);
	freez(lp->df_Referer);
	freez(lp->df_Uq);
	freez(lp->df_User_agent);
	freez(lp->df_b);
	freez(lp->df_h);
	freez(lp->df_m);
	freez(lp->df_s);
	freez(lp->df_u);
#undef freez
	lp->bogus = 0;

	return (reopen);
}

/*--------------------------------------------------------------------*/

static void
sighup(int sig)
{

	(void)sig;
	reopen = 1;
}

static FILE *
open_log(const char *ofn, int append)
{
	FILE *of;

	if ((of = fopen(ofn, append ? "a" : "w")) == NULL) {
		perror(ofn);
		exit(1);
	}
	return (of);
}

/*--------------------------------------------------------------------*/

static void
usage(void)
{

	fprintf(stderr, "usage: varnishncsa %s [-aDV] [-n varnish_name] [-P file] [-w file]\n", VSL_ARGS);
	exit(1);
}

int
main(int argc, char *argv[])
{
	int c;
	int a_flag = 0, D_flag = 0;
	const char *n_arg = NULL;
	const char *P_arg = NULL;
	const char *w_arg = NULL;
	struct pidfh *pfh = NULL;
	struct VSL_data *vd;
	FILE *of;

	vd = VSL_New();

	while ((c = getopt(argc, argv, VSL_ARGS "aDn:P:Vw:")) != -1) {
		switch (c) {
		case 'a':
			a_flag = 1;
			break;
		case 'D':
			D_flag = 1;
			break;
		case 'n':
			n_arg = optarg;
			break;
		case 'P':
			P_arg = optarg;
			break;
		case 'V':
			varnish_version("varnishncsa");
			exit(0);
		case 'w':
			w_arg = optarg;
			break;
		default:
			if (VSL_Arg(vd, c, optarg) > 0)
				break;
			usage();
		}
	}

	if (VSL_OpenLog(vd, n_arg))
		exit(1);

	if (P_arg && (pfh = vpf_open(P_arg, 0600, NULL)) == NULL) {
		perror(P_arg);
		exit(1);
	}

	if (D_flag && daemon(0, 0) == -1) {
		perror("daemon()");
		if (pfh != NULL)
			vpf_remove(pfh);
		exit(1);
	}

	if (pfh != NULL)
		vpf_write(pfh);

	if (w_arg) {
		of = open_log(w_arg, a_flag);
		signal(SIGHUP, sighup);
	} else {
		w_arg = "stdout";
		of = stdout;
	}

	while (VSL_Dispatch(vd, h_ncsa, of) >= 0) {
		if (fflush(of) != 0) {
			perror(w_arg);
			exit(1);
		}
		if (reopen && of != stdout) {
			fclose(of);
			of = open_log(w_arg, a_flag);
			reopen = 0;
		}
	}

	exit(0);
}
