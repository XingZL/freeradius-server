/*
 * radwho.c	Show who is logged in on the terminal servers.
 *		Can also be installed as fingerd on the UNIX
 *		machine RADIUS runs on.
 *
 * Version:	$Id$
 *
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, write to the Free Software
 *   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 * Copyright 2000  The FreeRADIUS server project
 * Copyright 2000  Alan DeKok <aland@ox.org>
 */

static const char rcsid[] =
"$Id$";

#include "autoconf.h"
#include "libradius.h"

#include <stdlib.h>
#include <string.h>
#include <pwd.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <ctype.h>
#ifdef HAVE_NETINET_IN_H
#include <netinet/in.h>
#endif

#include "sysutmp.h"
#include "radutmp.h"
#include "radiusd.h"
#include "conffile.h"

/*
 *	FIXME: put in header file.
 */
#define SYS_FINGER "/usr/bin/finger"
#define FINGER_DIR "/usr/local/lib/finger"

/*
 *	Header above output and format.
 */
static const char *hdr1 =
"Login      Name              What  TTY  When      From      Location";
static const char *ufmt1 = "%-10.10s %-17.17s %-5.5s %-4.4s %-9.9s %-9.9s %-.16s%s";
static const char *ufmt1r = "%s,%s,%s,%s,%s,%s,%s%s";
static const char *rfmt1 = "%-10.10s %-17.17s %-5.5s %s%-3d %-9.9s %-9.9s %-.19s%s";
static const char *rfmt1r = "%s,%s,%s,%s%d,%s,%s,%s%s";

static const char *hdr2 =
"Login      Port    What      When          From       Location";
static const char *ufmt2 = "%-10.10s %-6.6d %-7.7s %-13.13s %-10.10s %-.16s%s";
static const char *ufmt2r = "%s,%d,%s,%s,%s,%s%s";
static const char *rfmt2 = "%-10.10s %s%-5d  %-6.6s %-13.13s %-10.10s %-.28s%s";
static const char *rfmt2r = "%s,%s%d,%s,%s,%s,%s%s";

static const char *eol = "\n";
static int showname = -1;
static int showptype = 0;
static int showcid = 0;
int debug_flag = 0;
const char *progname = "radwho";
const char *radlog_dir = NULL;
const char *radutmp_file = NULL;

const char *radius_dir = NULL;
const char *radacct_dir = NULL;
const char *radlib_dir = NULL;
uint32_t myip = INADDR_ANY;
int log_stripped_names;

/*
 *	Global, for log.c to use.
 */
struct main_config_t mainconfig;

struct radutmp_config_t {
  char *radutmp_fn;
} radutmpconfig;

static CONF_PARSER module_config[] = {
  { "filename", PW_TYPE_STRING_PTR, 0, &radutmpconfig.radutmp_fn,  RADUTMP },
  { NULL, -1, 0, NULL, NULL }
};

/*
 *	Safe popen. Ugh.
 */
static FILE *safe_popen(const char *cmd, const char *mode)
{
	char		*p;
	char		buf[1024];

	/*
	 *	Change all suspect characters into a space.
	 */
	strncpy(buf, cmd, sizeof(buf));
	buf[sizeof(buf) - 1] = 0;
	for (p = buf; *p; p++) {
		if (isalnum((int) *p))
			continue;
		if (strchr("@%-_ \t+:,./", *p) == NULL)
			*p = ' ';
	}

	return popen(buf, mode);
}

/*
 *	Print a file from FINGER_DIR. If the file is executable,
 *	execute it instead. Return 0 if succesfull.
 */
static int ffile(const char *arg)
{
	FILE *fp;
	char fn[1024];
	int p = 0;
	char *s;

	snprintf(fn, sizeof(fn), "%s/%.32s", FINGER_DIR, arg);
	if (access(fn, X_OK) == 0) {
		p = 1;
		snprintf(fn, sizeof(fn), "exec %s/%.32s 2>&1", FINGER_DIR, arg);
		fp = safe_popen(fn, "r");
	} else fp = fopen(fn, "r");

	if (fp == NULL)
		return -1;

	while(fgets(fn, 1024, fp)) {
		if ((s = strchr(fn, '\n')) != NULL)
			*s = 0;
		fprintf(stdout, "%s\r\n", fn);
	}
	if (p)
		pclose(fp);
	else
		fclose(fp);
	fflush(stdout);
	return 0;
}


/*
 *	Execute the system finger and translate LF to CRLF.
 */
static void sys_finger(const char *l)
{
	FILE *fp;
	char fn[1024];
	char *p;

	if (ffile(l) == 0)
		exit(0);

	snprintf(fn, sizeof(fn), "exec %s %s", SYS_FINGER, l);
	if ((fp = safe_popen(fn, "r")) == NULL) {
		printf("popen: %s\r\n", strerror(errno));
		exit(1);
	}

	while(fgets(fn, 1024, fp)) {
		if ((p = strchr(fn, '\n')) != NULL)
			*p = 0;
		fprintf(stdout, "%s\r\n", fn);
	}
	pclose(fp);
	exit(0);
}


/*
 *	Get fullname of a user.
 */
static char *fullname(char *username)
{
	struct passwd *pwd;
	char *s;

	if ((pwd = getpwnam(username)) != NULL) {
		if ((s = strchr(pwd->pw_gecos, ',')) != NULL) *s = 0;
		return pwd->pw_gecos;
	}
	return username;
}

/*
 *	Return protocol type.
 */
static const char *proto(int id, int porttype)
{
	static char buf[8];

	if (showptype) {
		if (!strchr("ASITX", porttype))
			porttype = ' ';
		if (id == 'S')
			snprintf(buf, sizeof(buf), "SLP %c", porttype);
		else if (id == 'P')
			snprintf(buf, sizeof(buf), "PPP %c", porttype);
		else
			snprintf(buf, sizeof(buf), "shl %c", porttype);
		return buf;
	}
	if (id == 'S') return "SLIP";
	if (id == 'P') return "PPP";
	return "shell";
}

/*
 *	Return a time in the form day hh:mm
 */
static char *dotime(time_t t)
{
	char *s = ctime(&t);

	if (showname) {
		strncpy(s + 4, s + 11, 5);
		s[9] = 0;
	} else {
		strncpy(s + 4, s + 8, 8);
		s[12] = 0;
	}

	return s;
}

/*
 *	Shorten tty name.
 */
static const char *ttyshort(char *tty)
{
	static char tmp[16];

	if (tty[0] == '/') tty += 5;

	if (strncmp(tty, "tty", 3) == 0) {
		if (tty[3] >= '0' && tty[3] <= '9')
			snprintf(tmp, sizeof(tmp), "v%.14s", tty + 3);
		else
			snprintf(tmp, sizeof(tmp), "%.15s", tty + 3);
		return tmp;
	}
	if (strncmp(tty, "vc", 2) == 0) {
		snprintf(tmp, sizeof(tmp), "v.14%s", tty + 2);
		return tmp;
	}
	if (strncmp(tty, "cu", 2) == 0) {
		return tmp + 2;
	}
	return "??";
}


/*
 *	Print address of NAS.
 */
static const char *hostname(char *buf, size_t buflen, uint32_t ipaddr)
{
	if (ipaddr == 0 || ipaddr == (uint32_t)-1 || ipaddr == (uint32_t)-2)
		return "";
	return ip_hostname(buf, buflen, ipaddr);
}


/*
 *	Print usage message and exit.
 */
static void usage(int status)
{
	FILE *output = status?stderr:stdout;

	fprintf(output, "Usage: radwho [-d raddb] [-lhfnsipcr]\n");
	fprintf(output, "       -c: show caller ID, if available\n");
	fprintf(output, "       -d: set the raddb directory (default is %s)\n",
		RADIUS_DIR);
	fprintf(output, "       -f: give fingerd output\n");
	fprintf(output, "       -i: show session ID\n");
	fprintf(output, "       -n: no full name\n");
	fprintf(output, "       -p: show port type\n");
	fprintf(output, "       -r: output as raw data\n");
	fprintf(output, "       -s: show full name\n");
	fprintf(output, "       -S: hide shell users from radius\n");
	exit(status);
}


/*
 *	Main program, either pmwho or fingerd.
 */
int main(int argc, char **argv)
{
	CONF_SECTION *maincs, *cs;
	FILE *fp;
	struct radutmp rt;
	struct utmp ut;
	char inbuf[128];
	char othername[256];
	char nasname[1024];
	char session_id[sizeof(rt.session_id)+1];
	int fingerd = 0;
	int hideshell = 0;
	int showsid = 0;
	int rawoutput = 0;
	int radiusoutput = 1;	/* Radius attributes */
	char *p, *q;
	const char *portind;
	int c, portno;
	char buffer[2048];

	radius_dir = RADIUS_DIR;

	while((c = getopt(argc, argv, "d:flnsSipcr")) != EOF) switch(c) {
		case 'd':
			radius_dir = optarg;
			break;
		case 'f':
			fingerd++;
			showname = 0;
			break;
		case 'h':
			usage(0);
			break;
		case 'S':
			hideshell = 1;
			break;
		case 'n':
			showname = 0;
			break;
		case 's':
			showname = 1;
			break;
		case 'i':
			showsid = 1;
			break;
		case 'p':
			showptype = 1;
			break;
		case 'c':
			showcid = 1;
			showname = 1;
			break;
		case 'r':
			rawoutput = 1;
			break;
		case 'R':
			radiusoutput = 1;
			break;
		default:
			usage(1);
			break;
	}

	/*
	 *	Initialize mainconfig
	 */
	memset(&mainconfig, 0, sizeof(mainconfig));
	mainconfig.radlog_dest = RADLOG_STDOUT;

        /* Read radiusd.conf */
	snprintf(buffer, sizeof(buffer), "%.200s/radiusd.conf", radius_dir);
	maincs = conf_read(NULL, 0, buffer, NULL);
	if (!maincs) {
		fprintf(stderr, "%s: Error reading radiusd.conf.\n", argv[0]);
		exit(1);
	}

        /* Read the radutmp section of radiusd.conf */
        cs = cf_section_sub_find(cf_section_sub_find(maincs, "modules"), "radutmp");
        if(!cs) {
                fprintf(stderr, "%s: No configuration information in radutmp section of radiusd.conf!\n",
                        argv[0]);
                exit(1);
        }

	cf_section_parse(cs, NULL, module_config);

	/* Assign the correct path for the radutmp file */
	radutmp_file = radutmpconfig.radutmp_fn;

	/*
	 *	See if we are "fingerd".
	 */
	if (strstr(argv[0], "fingerd")) {
		fingerd++;
		eol = "\r\n";
		if (showname < 0) showname = 0;
	}
	if (showname < 0) showname = 1;

	if (fingerd) {
		/*
		 *	Read first line of the input.
		 */
		fgets(inbuf, 128, stdin);
		p = inbuf;
		while(*p == ' ' || *p == '\t') p++;
		if (*p == '/' && *(p + 1)) p += 2;
		while(*p == ' ' || *p == '\t') p++;
		for(q = p; *q && *q != '\r' && *q != '\n'; q++)
			;
		*q = 0;

		/*
		 *	See if we fingered a specific user.
		 */
		ffile("header");
		if (*p) sys_finger(p);
	}

	/*
	 *	Show the users logged in on the terminal server(s).
	 */
	if ((fp = fopen(radutmp_file, "r")) == NULL) {
		fprintf(stderr, "%s: Error reading %s: %s\n",
			progname, radutmp_file, strerror(errno));
		return 0;
	}

	/*
	 *	Don't print the headers if raw or RADIUS
	 */
	if (!rawoutput && !radiusoutput) {
		fputs(showname ? hdr1 : hdr2, stdout);
		fputs(eol, stdout);
	}

	/*
	 *	Read the file, printing out active entries.
	 */
	while (fread(&rt, sizeof(rt), 1, fp) == 1) {
		if (rt.type != P_LOGIN) continue; /* hide logout sessions */

		/*
		 *	We don't show shell users if we are
		 *	fingerd, as we have done that above.
		 */
		if (hideshell && !strchr("PCS", rt.proto))
			continue;
		
		memcpy(session_id, rt.session_id, sizeof(rt.session_id));
		session_id[sizeof(rt.session_id)] = 0;
		
		if (!rawoutput && rt.nas_port > (showname ? 999 : 99999)) {
			portind = ">";
			portno = (showname ? 999 : 99999);
		} else {
			portind = "S";
			portno = rt.nas_port;
		}

		/*
		 *	Show the fill name, or not.
		 */
		if (showname) {
			printf((rawoutput == 0? rfmt1: rfmt1r),
			       rt.login,
			       showcid ? rt.caller_id :
			       (showsid? session_id : fullname(rt.login)),
			       proto(rt.proto, rt.porttype),
			       portind, portno,
			       dotime(rt.time),
			       ip_hostname(nasname, sizeof(nasname), rt.nas_address),
			       hostname(othername, sizeof(othername), rt.framed_address), eol);
		} else {
			printf((rawoutput == 0? rfmt2: rfmt2r),
			       rt.login,
			       portind, portno,
			       proto(rt.proto, rt.porttype),
			       dotime(rt.time),
			       ip_hostname(nasname, sizeof(nasname), rt.nas_address),
			       hostname(othername, sizeof(othername), rt.framed_address),
			       eol);
		}
	}
	fclose(fp);

	return 0;
}

