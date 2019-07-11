/* ssi - server-side-includes CGI program
**
** Copyright (C) 1995  Jef Poskanzer <jef@mail.acme.com>
** All rights reserved.
**
** Redistribution and use in source and binary forms, with or without
** modification, are permitted provided that the following conditions
** are met:
** 1. Redistributions of source code must retain the above copyright
**    notice, this list of conditions and the following disclaimer.
** 2. Redistributions in binary form must reproduce the above copyright
**    notice, this list of conditions and the following disclaimer in the
**    documentation and/or other materials provided with the distribution.
**
** THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
** AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
** IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
** ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNERS OR CONTRIBUTORS BE
** LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
** CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
** SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
** INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
** CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
** ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
** THE POSSIBILITY OF SUCH DAMAGE.
*/

#include <config.h>

/* System headers */
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/types.h>

/* Local headers */
#include "libhttpd.h"
#include "merecat.h"
#include "match.h"


#define ST_GROUND 0
#define ST_LESSTHAN 1
#define ST_BANG 2
#define ST_MINUS1 3
#define ST_MINUS2 4

#define ERRMSG_DEFAULT "[an error occurred while processing this directive]"

static char *url;
static char *errmsg = NULL;
static char timefmt[100];
static int sizefmt;

#define SF_BYTES 0
#define SF_ABBREV 1
static struct stat sb;

static void read_file(char *vfilename, char *filename, FILE *fp);


static void send_response(char *title, char *fmt, ...)
{
	va_list ap;
	char *srv, *host, *port;

	printf("<!DOCTYPE html>\n"
	       "<html>\n"
	       " <head>\n"
	       "  <title>%s</title>\n"
	       "  <link rel=\"icon\" type=\"image/x-icon\" href=\"/icons/favicon.ico\">\n"
	       "%s"
	       " </head>\n"
	       " <body>\n"
	       "<div id=\"wrapper\" tabindex=\"-1\">\n"
	       "<h2>%s</h2>\n"
	       "<p>\n", title, httpd_css_default(), title);

	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);

	host = getenv("SERVER_NAME");
	port = getenv("SERVER_PORT");
	srv = getenv("SERVER_SOFTWARE");
	printf("</p>\n"
	       "<address>%s httpd at %s port %s</address>"
	       "</div>\n"
	       "</body></html>\n", srv, host, port);
}

static void internal_error(char *reason)
{
	char *title = "500 Internal Error";

	send_response(title, "Something unusual went wrong in a server-side "
		      "includes request:\n"
		      "<blockquote>\n"
		      "%s\n"
		      "</blockquote>\n", reason);
}


static void not_found(char *filename)
{
	char *title = "404 Not Found";

	send_response(title, "The requested server-side includes filename, %s,\n"
		      "does not seem to exist.", filename);
}

static void show_errmsg(void)
{
	if (!errmsg)
		return;

	fputs(errmsg, stdout);
}

static void not_found2(char *directive, char *tag, char *filename2)
{
	syslog(LOG_NOTICE, "The filename requested in a %s %s directive; %s, "
	       "does not seem to exist.", directive, tag, filename2);
	show_errmsg();
}


static void not_permitted(char *directive, char *tag, char *val)
{
	syslog(LOG_NOTICE, "The filename requested in the %s %s=%s directive, "
	       "is not allowed.", directive, tag, val);
	show_errmsg();
}


static void unknown_directive(char *filename, char *directive)
{
	syslog(LOG_NOTICE, "The requested server-side-includes filename, %s, "
	       "tried to use an unknown directive, %s.", filename, directive);
	show_errmsg();
}


static void unknown_tag(char *filename, char *directive, char *tag)
{
	syslog(LOG_NOTICE, "The requested server-side-includes filename, %s, "
	       "tried to use directive %s with an unknown tag, %s.", filename,
	       directive, tag);
	show_errmsg();
}


static void unknown_value(char *filename, char *directive, char *tag, char *val)
{
	syslog(LOG_NOTICE, "The requested server-side-includes filename, %s, "
	       "tried to use directive %s %s with an unknown value, %s.",
	       filename, directive, tag, val);
	show_errmsg();
}


static int get_filename(char *vfilename, char *filename, char *directive, char *tag, char *val, char *fn, int fnsize)
{
	int vl, fl;
	char *cp;

	/* Used for the various commands that accept a file name.
	 ** These commands accept two tags:
	 **   virtual
	 **     Gives a virtual path to a document on the server.
	 **   file
	 **     Gives a pathname relative to the current directory. ../ cannot
	 **     be used in this pathname, nor can absolute paths be used.
	 */
	vl = strlen(vfilename);
	fl = strlen(filename);
	if (strcmp(tag, "virtual") == 0) {
		if (strstr(val, "../") != (char *)0) {
			not_permitted(directive, tag, val);
			return -1;
		}
		/* Figure out root using difference between vfilename and filename. */
		if (vl > fl || strcmp(vfilename, &filename[fl - vl]) != 0)
			return -1;
		if (fl - vl + strlen(val) >= fnsize)
			return -1;
		strncpy(fn, filename, fl - vl);
		strcpy(&fn[fl - vl], val);
	} else if (strcmp(tag, "file") == 0) {
		if (val[0] == '/' || strstr(val, "../") != (char *)0) {
			not_permitted(directive, tag, val);
			return -1;
		}
		if (fl + 1 + strlen(val) >= fnsize)
			return -1;
		strcpy(fn, filename);
		cp = strrchr(fn, '/');
		if (cp == (char *)0) {
			cp = &fn[strlen(fn)];
			*cp = '/';
		}
		strcpy(++cp, val);
	} else {
		unknown_tag(filename, directive, tag);
		return -1;
	}
	return 0;
}


static int check_filename(char *filename)
{
	static int inited = 0;
	static char *cgi_pattern;
	int fnl;
	char *cp;
	char *dirname;
	char *authname;
	struct stat sb;
	int r;

	if (!inited) {
		/* Get the cgi pattern. */
		cgi_pattern = getenv("CGI_PATTERN");
#ifdef CGI_PATTERN
		if (cgi_pattern == (char *)0)
			cgi_pattern = CGI_PATTERN;
#endif				/* CGI_PATTERN */
		inited = 1;
	}

	/* ../ is not permitted. */
	if (strstr(filename, "../") != (char *)0)
		return 0;

#ifdef AUTH_FILE
	/* Ensure that we are not reading a basic auth password file. */
	fnl = strlen(filename);
	if (strcmp(filename, AUTH_FILE) == 0 ||
	    (fnl >= sizeof(AUTH_FILE) &&
	     strcmp(&filename[fnl - sizeof(AUTH_FILE) + 1], AUTH_FILE) == 0 && filename[fnl - sizeof(AUTH_FILE)] == '/'))
		return 0;

	/* Check for an auth file in the same directory.  We can't do an actual
	 ** auth password check here because CGI programs are not given the
	 ** authorization header, for security reasons.  So instead we just
	 ** prohibit access to all auth-protected files.
	 */
	dirname = strdup(filename);
	if (dirname == (char *)0)
		return 0;	/* out of memory */
	cp = strrchr(dirname, '/');
	if (cp == (char *)0)
		strcpy(dirname, ".");
	else
		*cp = '\0';
	authname = malloc(strlen(dirname) + 1 + sizeof(AUTH_FILE));
	if (authname == (char *)0) {
		free(dirname);
		return 0;	/* out of memory */
	}
	sprintf(authname, "%s/%s", dirname, AUTH_FILE);
	r = stat(authname, &sb);
	free(dirname);
	free(authname);
	if (r == 0)
		return 0;
#endif				/* AUTH_FILE */

	/* Ensure that we are not reading a CGI file. */
	if (cgi_pattern != (char *)0 && match(cgi_pattern, filename))
		return 0;

	return 1;
}


static void show_time(time_t t, int gmt)
{
	struct tm *tmP;
	char tbuf[500];

	if (gmt)
		tmP = gmtime(&t);
	else
		tmP = localtime(&t);
	if (strftime(tbuf, sizeof(tbuf), timefmt, tmP) > 0)
		fputs(tbuf, stdout);
}


static void show_size(off_t size)
{
	switch (sizefmt) {
	case SF_BYTES:
		printf("%ld", (long)size);	/* spec says should have commas */
		break;
	case SF_ABBREV:
		if (size < 1024)
			printf("%ld", (long)size);
		else if (size < 1024)
			printf("%ldK", (long)size / 1024L);
		else if (size < 1024 * 1024)
			printf("%ldM", (long)size / (1024L * 1024L));
		else
			printf("%ldG", (long)size / (1024L * 1024L * 1024L));
		break;
	}
}


static void do_config(char *vfilename, char *filename, FILE *fp, char *directive, char *tag, char *val)
{
	/* The config directive controls various aspects of the file parsing.
	 ** There are two valid tags:
	 **   timefmt
	 **     Gives the server a new format to use when providing dates.  This
	 **     is a string compatible with the strftime library call.
	 **   sizefmt
	 **     Determines the formatting to be used when displaying the size of
	 **     a file.  Valid choices are bytes, for a formatted byte count
	 **     (formatted as 1,234,567), or abbrev for an abbreviated version
	 **     displaying the number of kilobytes or megabytes the file occupies.
	 */

	if (strcmp(tag, "timefmt") == 0) {
		strncpy(timefmt, val, sizeof(timefmt) - 1);
		timefmt[sizeof(timefmt) - 1] = '\0';
	} else if (strcmp(tag, "sizefmt") == 0) {
		if (strcmp(val, "bytes") == 0)
			sizefmt = SF_BYTES;
		else if (strcmp(val, "abbrev") == 0)
			sizefmt = SF_ABBREV;
		else
			unknown_value(filename, directive, tag, val);
	} else if (strcmp(tag, "errmsg") == 0) {
		free(errmsg);
		errmsg = strdup(val);
	} else
		unknown_tag(filename, directive, tag);
}


static void do_include(char *vfilename, char *filename, FILE *fp, char *directive, char *tag, char *val)
{
	char vfilename2[1000];
	char filename2[1000];
	FILE *fp2;

	/* Inserts the text of another document into the parsed document. */

	if (get_filename(vfilename, filename, directive, tag, val, filename2, sizeof(filename2)) < 0)
		return;

	if (!check_filename(filename2)) {
		not_permitted(directive, tag, filename2);
		return;
	}

	fp2 = fopen(filename2, "r");
	if (fp2 == (FILE *)0) {
		not_found2(directive, tag, filename2);
		return;
	}

	if (strcmp(tag, "virtual") == 0) {
		if (strlen(val) < sizeof(vfilename2))
			strcpy(vfilename2, val);
		else
			strcpy(vfilename2, filename2);	/* same size, has to fit */
	} else {
		if (strlen(vfilename) + 1 + strlen(val) < sizeof(vfilename2)) {
			char *cp;

			strcpy(vfilename2, vfilename);
			cp = strrchr(vfilename2, '/');
			if (cp == (char *)0) {
				cp = &vfilename2[strlen(vfilename2)];
				*cp = '/';
			}
			strcpy(++cp, val);
		} else
			strcpy(vfilename2, filename2);	/* same size, has to fit */
	}

	read_file(vfilename2, filename2, fp2);
	fclose(fp2);
}


static void do_echo(char *vfilename, char *filename, FILE *fp, char *directive, char *tag, char *val)
{
	char *cp;
	time_t t;

	/* Prints the value of one of the include variables.  Any dates are
	 ** printed subject to the currently configured timefmt.  The only valid
	 ** tag is var, whose value is the name of the variable you wish to echo.
	 */

	if (strcmp(tag, "var") != 0)
		unknown_tag(filename, directive, tag);
	else {
		if (strcmp(val, "DOCUMENT_NAME") == 0) {
			/* The current filename. */
			fputs(filename, stdout);
		} else if (strcmp(val, "DOCUMENT_URI") == 0) {
			/* The virtual path to this file (such as /~robm/foo.shtml). */
			fputs(vfilename, stdout);
		} else if (strcmp(val, "QUERY_STRING_UNESCAPED") == 0) {
			/* The unescaped version of any search query the client sent. */
			cp = getenv("QUERY_STRING");
			if (cp != (char *)0)
				fputs(cp, stdout);
		} else if (strcmp(val, "DATE_LOCAL") == 0) {
			/* The current date, local time zone. */
			t = time((time_t *)0);
			show_time(t, 0);
		} else if (strcmp(val, "DATE_GMT") == 0) {
			/* Same as DATE_LOCAL but in Greenwich mean time. */
			t = time((time_t *)0);
			show_time(t, 1);
		} else if (strcmp(val, "LAST_MODIFIED") == 0) {
			/* The last modification date of the current document. */
			if (fstat(fileno(fp), &sb) >= 0)
				show_time(sb.st_mtime, 0);
		} else {
			/* Try an environment variable. */
			cp = getenv(val);
			if (cp == (char *)0)
				unknown_value(filename, directive, tag, val);
			else
				fputs(cp, stdout);
		}
	}
}


static void do_fsize(char *vfilename, char *filename, FILE *fp, char *directive, char *tag, char *val)
{
	char filename2[1000];

	/* Prints the size of the specified file. */

	if (get_filename(vfilename, filename, directive, tag, val, filename2, sizeof(filename2)) < 0)
		return;
	if (stat(filename2, &sb) < 0) {
		not_found2(directive, tag, filename2);
		return;
	}
	show_size(sb.st_size);
}


static void do_flastmod(char *vfilename, char *filename, FILE *fp, char *directive, char *tag, char *val)
{
	char filename2[1000];

	/* Prints the last modification date of the specified file. */

	if (get_filename(vfilename, filename, directive, tag, val, filename2, sizeof(filename2)) < 0)
		return;
	if (stat(filename2, &sb) < 0) {
		not_found2(directive, tag, filename2);
		return;
	}
	show_time(sb.st_mtime, 0);
}


static void parse(char *vfilename, char *filename, FILE *fp, char *str)
{
	char *directive;
	char *cp;
	int ntags;
	char *tags[200];
	int dirn;

#define DI_CONFIG 0
#define DI_INCLUDE 1
#define DI_ECHO 2
#define DI_FSIZE 3
#define DI_FLASTMOD 4
	int i;
	char *val;

	directive = str;
	directive += strspn(directive, " \t\n\r");

	ntags = 0;
	cp = directive;
	for (;;) {
		cp = strpbrk(cp, " \t\n\r\"");
		if (cp == (char *)0)
			break;
		if (*cp == '"') {
			cp = strpbrk(cp + 1, "\"");
			++cp;
			if (*cp == '\0')
				break;
		}
		*cp++ = '\0';
		cp += strspn(cp, " \t\n\r");
		if (*cp == '\0')
			break;
		if (ntags < sizeof(tags) / sizeof(*tags))
			tags[ntags++] = cp;
	}

	if (strcmp(directive, "config") == 0)
		dirn = DI_CONFIG;
	else if (strcmp(directive, "include") == 0)
		dirn = DI_INCLUDE;
	else if (strcmp(directive, "echo") == 0)
		dirn = DI_ECHO;
	else if (strcmp(directive, "fsize") == 0)
		dirn = DI_FSIZE;
	else if (strcmp(directive, "flastmod") == 0)
		dirn = DI_FLASTMOD;
	else {
		unknown_directive(filename, directive);
		return;
	}

	for (i = 0; i < ntags; ++i) {
		if (i > 0)
			putchar(' ');
		val = strchr(tags[i], '=');
		if (val == (char *)0)
			val = "";
		else
			*val++ = '\0';
		if (*val == '"' && val[strlen(val) - 1] == '"') {
			val[strlen(val) - 1] = '\0';
			++val;
		}
		switch (dirn) {
		case DI_CONFIG:
			do_config(vfilename, filename, fp, directive, tags[i], val);
			break;
		case DI_INCLUDE:
			do_include(vfilename, filename, fp, directive, tags[i], val);
			break;
		case DI_ECHO:
			do_echo(vfilename, filename, fp, directive, tags[i], val);
			break;
		case DI_FSIZE:
			do_fsize(vfilename, filename, fp, directive, tags[i], val);
			break;
		case DI_FLASTMOD:
			do_flastmod(vfilename, filename, fp, directive, tags[i], val);
			break;
		}
	}
}


static void slurp(char *vfilename, char *filename, FILE *fp)
{
	char buf[1000];
	int i;
	int state;
	int ich;

	/* Now slurp in the rest of the comment from the input file. */
	i = 0;
	state = ST_GROUND;
	while ((ich = getc(fp)) != EOF) {
		switch (state) {
		case ST_GROUND:
			if (ich == '-')
				state = ST_MINUS1;
			break;
		case ST_MINUS1:
			if (ich == '-')
				state = ST_MINUS2;
			else
				state = ST_GROUND;
			break;
		case ST_MINUS2:
			if (ich == '>') {
				buf[i - 2] = '\0';
				parse(vfilename, filename, fp, buf);
				return;
			} else if (ich != '-')
				state = ST_GROUND;
			break;
		}
		if (i < sizeof(buf) - 1)
			buf[i++] = (char)ich;
	}
}


static void read_file(char *vfilename, char *filename, FILE *fp)
{
	int ich;
	int state;

	/* Copy it to output, while running a state-machine to look for
	 ** SSI directives.
	 */
	state = ST_GROUND;
	while ((ich = getc(fp)) != EOF) {
		switch (state) {
		case ST_GROUND:
			if (ich == '<') {
				state = ST_LESSTHAN;
				continue;
			}
			break;
		case ST_LESSTHAN:
			if (ich == '!') {
				state = ST_BANG;
				continue;
			} else {
				state = ST_GROUND;
				putchar('<');
			}
			break;
		case ST_BANG:
			if (ich == '-') {
				state = ST_MINUS1;
				continue;
			} else {
				state = ST_GROUND;
				fputs("<!", stdout);
			}
			break;
		case ST_MINUS1:
			if (ich == '-') {
				state = ST_MINUS2;
				continue;
			} else {
				state = ST_GROUND;
				fputs("<!-", stdout);
			}
			break;
		case ST_MINUS2:
			if (ich == '#') {
				slurp(vfilename, filename, fp);
				state = ST_GROUND;
				continue;
			} else {
				state = ST_GROUND;
				fputs("<!--", stdout);
			}
			break;
		}
		putchar((char)ich);
	}
}


int main(int argc, char **argv)
{
	char *script_name;
	char *path_info;
	char *path_translated;
	FILE *fp;

	/* Default formats. */
	strcpy(timefmt, "%a %b %e %T %Z %Y");
	sizefmt = SF_BYTES;

	if (!getenv("SILENT_ERRORS"))
		errmsg = strdup(ERRMSG_DEFAULT);
	else
		unsetenv("SILENT_ERRORS");

	/* The MIME type has to be text/html. */
	fputs("Content-type: text/html\n\n", stdout);

	/* Get the name that we were run as. */
	script_name = getenv("SCRIPT_NAME");
	if (script_name == (char *)0) {
		internal_error("Couldn't get SCRIPT_NAME environment variable.");
		exit(1);
	}

	/* Append the PATH_INFO, if any, to get the full URL. */
	path_info = getenv("PATH_INFO");
	if (path_info == (char *)0)
		path_info = "";
	url = (char *)malloc(strlen(script_name) + strlen(path_info) + 1);
	if (url == (char *)0) {
		internal_error("Out of memory.");
		exit(1);
	}
	sprintf(url, "%s%s", script_name, path_info);

	/* Get the name of the file to parse. */
	path_translated = getenv("PATH_TRANSLATED");
	if (path_translated == (char *)0) {
		internal_error("Couldn't get PATH_TRANSLATED environment variable.");
		exit(1);
	}

	if (!check_filename(path_translated)) {
		not_permitted("initial", "PATH_TRANSLATED", path_translated);
		exit(1);
	}

	/* Open it. */
	fp = fopen(path_translated, "r");
	if (fp == (FILE *)0) {
		not_found(path_translated);
		exit(1);
	}

	/* Read and handle the file. */
	read_file(path_info, path_translated, fp);

	fclose(fp);
	exit(0);
}
