/*
    dtach - A simple program that emulates the detach feature of screen.
    Copyright (C) 2004-2008 Ned T. Crigler, 2011 Devin J. Pohly

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
*/
#include "dtach.h"

/* Make sure the binary has a copyright. */
const char copyright[] =
	"dtattach - version " PACKAGE_VERSION ", compiled on " BUILD_DATE ".\n"
	" (C) Copyright 2004-2008 Ned T. Crigler, 2011 Devin J. Pohly\n";

#ifndef VDISABLE
#ifdef _POSIX_VDISABLE
#define VDISABLE _POSIX_VDISABLE
#else
#define VDISABLE 0377
#endif
#endif

/* argv[0] from the program */
char *progname;
/* The name of the passed in socket. */
char *sockname;
/* Flag to signal end of process. */
int stop;
/* The character used for detaching. Defaults to '^\' */
int detach_char = '\\' - 64;
/* 1 if we should not interpret the suspend character. */
int no_suspend;
/* The default redraw method. Initially set to unspecified. */
int redraw_method = REDRAW_UNSPEC;

/* The original terminal settings, for restoring later. */
struct termios orig_term;

/*
** The current terminal settings. After coming back from a suspend, we
** restore this.
*/
static struct termios cur_term;
/* 1 if the window size changed */
static int win_changed;

static void
usage()
{
	printf(
		"Usage: dtattach <socket> <options>\n"
		"Options:\n"
		"  -e <char>\tSet the detach character to <char>. Defaults "
		"to ^\\.\n"
		"\t\t  Use \"\" to disable.\n"
		"  -r <method>\tSet the redraw method to <method>. The "
		"valid methods are:\n"
		"\t\t     none: Don't redraw at all.\n"
		"\t\t   ctrl_l: Send a Ctrl L character to the program.\n"
		"\t\t    winch: Send a WINCH signal to the program.\n"
		"  -z\t\tDisable processing of the suspend key.\n"
		"\nReport any bugs to <%s>.\n", PACKAGE_BUGREPORT);
}

/* Restores the original terminal settings. */
static void
restore_term(void)
{
	tcsetattr(0, TCSADRAIN, &orig_term);
}

/* Connects to a unix domain socket */
static int
connect_socket(char *name)
{
	int s;
	struct sockaddr_un sockun;

	s = socket(PF_UNIX, SOCK_STREAM, 0);
	if (s < 0)
		return -1;
	sockun.sun_family = AF_UNIX;
	strcpy(sockun.sun_path, name);
	if (connect(s, (struct sockaddr*)&sockun, sizeof(sockun)) < 0)
	{
		close(s);

		/* ECONNREFUSED is also returned for regular files, so make
		** sure we are trying to connect to a socket. */
		if (errno == ECONNREFUSED)
		{
			struct stat st;

			if (stat(name, &st) < 0)
				return -1;
			else if (!S_ISSOCK(st.st_mode) || S_ISREG(st.st_mode))
				errno = ENOTSOCK;
		}
		return -1;
	}
	return s;
}

/* Signal */
static RETSIGTYPE
die(int sig)
{
	/* Print a nice pretty message for some things. */
	if (sig != SIGHUP && sig != SIGINT)
	{
		fprintf(stderr, "got signal %d\n", sig);
		exit(128 + sig);
	}

	fprintf(stderr, "detached\n");
	stop = 1;
}

/* Window size change. */
static RETSIGTYPE
win_change()
{
	win_changed = 1;
}

/* Handles input from the keyboard. */
static void
process_kbd(int s, struct packet *pkt)
{
	/* Suspend? */
	if (!no_suspend && (pkt->u.buf[0] == cur_term.c_cc[VSUSP]))
	{
		/* Tell the master that we are suspending. */
		pkt->type = MSG_DETACH;
		write(s, pkt, sizeof(struct packet));

		/* And suspend... */
		tcsetattr(0, TCSADRAIN, &orig_term);
		printf("\r\n");
		kill(getpid(), SIGTSTP);
		tcsetattr(0, TCSADRAIN, &cur_term);

		/* Tell the master that we are returning. */
		pkt->type = MSG_ATTACH;
		write(s, pkt, sizeof(struct packet));

		/* We would like a redraw, too. */
		pkt->type = MSG_REDRAW;
		pkt->len = redraw_method;
		ioctl(0, TIOCGWINSZ, &pkt->u.ws);
		write(s, pkt, sizeof(struct packet));
		return;
	}
	/* Detach char? */
	else if (pkt->u.buf[0] == detach_char)
	{
		fprintf(stderr, "detached\n");
		stop = 1;
		return;
	}
	/* Just in case something pukes out. */
	else if (pkt->u.buf[0] == '\f')
		win_changed = 1;

	/* Push it out */
	write(s, pkt, sizeof(struct packet));
}

static int
attach_main()
{
	struct packet pkt;
	unsigned char buf[BUFSIZE];
	fd_set readfds;
	int s;

	/* Attempt to open the socket. */
	s = connect_socket(sockname);
	if (s < 0)
	{
		fprintf(stderr, "%s: %s: %s\n", progname, sockname,
			strerror(errno));
		return (errno == ECONNREFUSED) ? 2 : 1;
	}

	/* The current terminal settings are equal to the original terminal
	** settings at this point. */
	cur_term = orig_term;

	/* Set a trap to restore the terminal when we die. */
	atexit(restore_term);

	/* Mask out our signals. */
	sigset_t sigs;
	sigemptyset(&sigs);
	sigaddset(&sigs, SIGHUP);
	sigaddset(&sigs, SIGTERM);
	sigaddset(&sigs, SIGINT);
	sigaddset(&sigs, SIGQUIT);
	sigaddset(&sigs, SIGWINCH);
	sigprocmask(SIG_SETMASK, &sigs, NULL);
	sigemptyset(&sigs);

	/* Register signal handlers. */
	struct sigaction sa;
	sa.sa_flags = 0;
	sa.sa_mask = sigs;

	sa.sa_handler = SIG_IGN;
	sigaction(SIGPIPE, &sa, NULL);
	sigaction(SIGXFSZ, &sa, NULL);

	sa.sa_handler = die;
	sigaction(SIGHUP, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGQUIT, &sa, NULL);

	sa.sa_handler = win_change;
	sigaction(SIGWINCH, &sa, NULL);

	/* Set raw mode. */
	cur_term.c_iflag &= ~(IGNBRK|BRKINT|PARMRK|ISTRIP|INLCR|IGNCR|ICRNL);
	cur_term.c_iflag &= ~(IXON|IXOFF);
	cur_term.c_oflag &= ~(OPOST);
	cur_term.c_lflag &= ~(ECHO|ECHONL|ICANON|ISIG|IEXTEN);
	cur_term.c_cflag &= ~(CSIZE|PARENB);
	cur_term.c_cflag |= CS8;
	cur_term.c_cc[VLNEXT] = VDISABLE;
	cur_term.c_cc[VMIN] = 1;
	cur_term.c_cc[VTIME] = 0;
	tcsetattr(0, TCSADRAIN, &cur_term);

	/* Tell the master that we want to attach. */
	pkt.type = MSG_ATTACH;
	write(s, &pkt, sizeof(struct packet));

	/* We would like a redraw, too. */
	pkt.type = MSG_REDRAW;
	pkt.len = redraw_method;
	ioctl(0, TIOCGWINSZ, &pkt.u.ws);
	write(s, &pkt, sizeof(struct packet));

	/* Wait for things to happen */
	stop = 0;
	while (1)
	{
		int n;

		FD_ZERO(&readfds);
		FD_SET(0, &readfds);
		FD_SET(s, &readfds);
		n = pselect(s + 1, &readfds, NULL, NULL, NULL, &sigs);
		if (n < 0 && errno != EINTR && errno != EAGAIN)
		{
			fprintf(stderr, "select failed\n");
			return 1;
		}

		/* Pty activity */
		if (n > 0 && FD_ISSET(s, &readfds))
		{
			int len = read(s, buf, sizeof(buf));

			if (len == 0)
			{
				fprintf(stderr, "EOF - dtach terminating\n");
				return 0;
			}
			else if (len < 0)
			{
				fprintf(stderr, "read returned an error\n");
				return 1;
			}
			/* Send the data to the terminal. */
			write(1, buf, len);
			n--;
		}
		/* stdin activity */
		if (n > 0 && FD_ISSET(0, &readfds))
		{
			pkt.type = MSG_PUSH;
			memset(pkt.u.buf, 0, sizeof(pkt.u.buf));
			pkt.len = read(0, pkt.u.buf, sizeof(pkt.u.buf));

			if (pkt.len <= 0)
				return 1;
			process_kbd(s, &pkt);
			n--;
		}
		if (stop)
			break;

		/* Window size changed? */
		if (win_changed)
		{
			win_changed = 0;

			pkt.type = MSG_WINCH;
			ioctl(0, TIOCGWINSZ, &pkt.u.ws);
			write(s, &pkt, sizeof(pkt));
		}
	}
	return 0;
}

int
main(int argc, char **argv)
{
	/* Save the program name */
	progname = argv[0];
	++argv; --argc;

	/* Parse the arguments */
	if (argc < 1)
	{
		fprintf(stderr, "%s: No socket was specified.\n", progname);
		fprintf(stderr, "Try '%s --help' for more information.\n",
			progname);
		return 1;
	}

	if (strcmp(*argv, "--help") == 0 || strcmp(*argv, "-?") == 0)
	{
		usage();
		return 0;
	}
	else if (strcmp(*argv, "--version") == 0)
	{
		printf("%s", copyright);
		return 0;
	}
	sockname = *argv;
	++argv; --argc;

	while (argc >= 1 && **argv == '-')
	{
		char *p;

		for (p = *argv + 1; *p; ++p)
		{
			if (*p == 'z')
				no_suspend = 1;
			else if (*p == 'e')
			{
				++argv; --argc;
				if (argc < 1)
				{
					fprintf(stderr, "%s: No escape "
						"character specified.\n",
						progname);	
					fprintf(stderr, "Try '%s --help' for "
						"more information.\n",
						progname);
					return 1;
				}
				if (argv[0][0] == '^' && argv[0][1])
				{
					if (argv[0][1] == '?')
						detach_char = '\177';
					else
						detach_char = argv[0][1] & 037;
				}
				else if (!argv[0][0])
					detach_char = -1;
				else
					detach_char = argv[0][0];
				break;
			}
			else if (*p == 'r')
			{
				++argv; --argc;
				if (argc < 1)
				{
					fprintf(stderr, "%s: No redraw method "
						"specified.\n", progname);	
					fprintf(stderr, "Try '%s --help' for "
						"more information.\n",
						progname);
					return 1;
				}
				if (strcmp(argv[0], "none") == 0)
					redraw_method = REDRAW_NONE;
				else if (strcmp(argv[0], "ctrl_l") == 0)
					redraw_method = REDRAW_CTRL_L;
				else if (strcmp(argv[0], "winch") == 0)
					redraw_method = REDRAW_WINCH;
				else
				{
					fprintf(stderr, "%s: Invalid redraw "
						"method specified.\n",
						progname);
					fprintf(stderr, "Try '%s --help' for "
						"more information.\n",
						progname);
					return 1;
				}
				break;
			}
			else if (*p == '?')
			{
				usage();
				return 0;
			}
			else
			{
				fprintf(stderr, "%s: Invalid option '-%c'\n",
					progname, *p);
				fprintf(stderr, "Try '%s --help' for more "
					"information.\n", progname);
				return 1;
			}
		}
		++argv; --argc;
	}

	if (argc > 0)
	{
		fprintf(stderr, "%s: Invalid number of arguments.\n",
			progname);
		fprintf(stderr, "Try '%s --help' for more information.\n",
			progname);
		return 1;
	}

	/* Save the original terminal settings. */
	if (tcgetattr(0, &orig_term) < 0)
		memset(&orig_term, 0, sizeof(struct termios));

	return attach_main();
}
