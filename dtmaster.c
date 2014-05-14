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
	"dtmaster - version " PACKAGE_VERSION ", compiled on " BUILD_DATE ".\n"
	" (C) Copyright 2004-2008 Ned T. Crigler, 2011 Devin J. Pohly.\n";

/* argv[0] from the program */
char *progname;
/* The name of the passed in socket. */
char *sockname;
/* Flag to signal end of process. */
int stop;
/* The default redraw method. If unspecified, use SIGWINCH. */
int redraw_method = REDRAW_CTRL_L;

/* The original terminal settings, for initializing the pty. */
struct termios orig_term;
int dont_have_tty;

/* The pty struct - The pty information is stored here. */
struct pty
{
	/* File descriptor of the pty */
	int fd;
#ifdef BROKEN_MASTER
	/* File descriptor of the slave side of the pty. For broken systems. */
	int slave;
#endif
	/* Process id of the child. */
	pid_t pid;
	/* The terminal parameters of the pty. Old and new for comparision
	** purposes. */
	struct termios term;
	/* The current window size of the pty. */
	struct winsize ws;
};

/* A connected client */
struct client
{
	/* The next client in the linked list. */
	struct client *next;
	/* The previous client in the linked list. */
	struct client **pprev;
	/* File descriptor of the client. */
	int fd;
	/* Whether or not the client is attached. */
	int attached;
};

/* The list of connected clients. */
static struct client *clients;
/* The pseudo-terminal created for the child process. */
static struct pty the_pty;

#ifndef HAVE_FORKPTY
pid_t forkpty(int *amaster, char *name, struct termios *termp,
	struct winsize *winp);
#endif

static void
usage()
{
	printf(
		"Usage: dtmaster <socket> <options> <command> [arg...]\n"
		"Options:\n"
		"  -n\t\tDo not fork after running the command.\n"
		"  -w\t\tWait for a client to attach before processing any "
		"output\n"
		"\t\t  from the command.\n"
		"  -r <method>\tSet the default redraw method to <method>. The "
		"valid\n"
		"\t\t  methods are:\n"
		"\t\t     none: Don't redraw at all.\n"
		"\t\t   ctrl_l: Send a Ctrl-L character to the program.\n"
		"\t\t    winch: Send SIGWINCH to the program.\n"
		"\nReport any bugs to <%s>.\n", PACKAGE_BUGREPORT);
}

/* Unlink the socket */
static void
unlink_socket(void)
{
	unlink(sockname);
}

/* Signal */
static RETSIGTYPE 
die(int sig)
{
	/* Well, the child died. */
	if (sig == SIGCHLD)
	{
#ifdef BROKEN_MASTER
		/* Damn you Solaris! */
		close(the_pty.fd);
#endif
		return;
	}
	exit(128 + sig);
}

/* Sets a file descriptor to non-blocking mode. */
static int
setnonblocking(int fd)
{
	int flags;

#if defined(O_NONBLOCK)
	flags = fcntl(fd, F_GETFL);
	if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0)
		return -1;
	return 0;
#elif defined(FIONBIO)
	flags = 1;
	if (ioctl(fd, FIONBIO, &flags) < 0)
		return -1;
	return 0;
#else
#warning Do not know how to set non-blocking mode.
	return 0;
#endif
}

/* Initialize the pty structure. */
static int
init_pty(char **argv, int statusfd)
{
	/* Use the original terminal's settings. We don't have to set the
	** window size here, because the attacher will send it in a packet. */
	the_pty.term = orig_term;
	memset(&the_pty.ws, 0, sizeof(struct winsize));

	/* Create the pty process */
	if (!dont_have_tty)
		the_pty.pid = forkpty(&the_pty.fd, NULL, &the_pty.term, NULL);
	else
		the_pty.pid = forkpty(&the_pty.fd, NULL, NULL, NULL);
	if (the_pty.pid < 0)
		return -1;
	else if (the_pty.pid == 0)
	{
		/* Child.. Execute the program. */
		execvp(*argv, argv);

		/* Report the error to statusfd if we can, or stdout if we
		** can't. */
		if (statusfd != -1)
			dup2(statusfd, 1);
		else
			fprintf(stderr, "\r\n");

		fprintf(stderr, "%s: could not execute %s: %s\r\n", progname,
		       *argv, strerror(errno));
		fflush(stdout);
		_exit(127);
	}
	/* Parent.. Finish up and return */
#ifdef BROKEN_MASTER
	{
		char *buf;

		buf = ptsname(the_pty.fd);
		the_pty.slave = open(buf, O_RDWR|O_NOCTTY);
	}
#endif
	return 0;
}

/* Send a signal to the slave side of a pseudo-terminal. */
static void
killpty(struct pty *pty, int sig)
{
	pid_t pgrp = -1;

#ifdef TIOCSIGNAL
	if (ioctl(pty->fd, TIOCSIGNAL, sig) >= 0)
		return;
#endif
#ifdef TIOCSIG
	if (ioctl(pty->fd, TIOCSIG, sig) >= 0)
		return;
#endif
#ifdef TIOCGPGRP
#ifdef BROKEN_MASTER
	if (ioctl(pty->slave, TIOCGPGRP, &pgrp) >= 0 && pgrp != -1 &&
		kill(-pgrp, sig) >= 0)
		return;
#endif
	if (ioctl(pty->fd, TIOCGPGRP, &pgrp) >= 0 && pgrp != -1 &&
		kill(-pgrp, sig) >= 0)
		return;
#endif

	/* Fallback using the child's pid. */
	kill(-pty->pid, sig);
}

/* Creates a new unix domain socket. */
static int
create_socket(char *name)
{
	int s;
	struct sockaddr_un sockun;

	s = socket(PF_UNIX, SOCK_STREAM, 0);
	if (s < 0)
		return -1;
	init_sockaddr_un(&sockun, name);
	if (bind(s, (struct sockaddr*)&sockun, sizeof(sockun)) < 0)
	{
		close(s);
		return -1;
	}
	if (listen(s, 128) < 0)
	{
		close(s);
		return -1;
	}
	if (setnonblocking(s) < 0)
	{
		close(s);
		return -1;
	}
	/* chmod it to prevent any suprises */
	if (chmod(name, 0600) < 0)
	{
		close(s);
		return -1;
	}
	return s;
}

/* Process activity on the pty - Input and terminal changes are sent out to
** the attached clients. If the pty goes away, we die. */
static int
pty_activity(int s)
{
	unsigned char buf[BUFSIZE];
	ssize_t len;
	struct client *p;
	fd_set readfds, writefds;
	int highest_fd, nclients;

	/* Read the pty activity */
	len = read(the_pty.fd, buf, sizeof(buf));

	/* Error or zero read */
	if (len < 0 && errno == EIO)
	{
		/* EIO can mean pty slave is closed, which is OK. */
		stop = 1;
		return 0;
	}
	else if (len <= 0)
		return 1;

#ifdef BROKEN_MASTER
	/* Get the current terminal settings. */
	if (tcgetattr(the_pty.slave, &the_pty.term) < 0)
		return 1;
#else
	/* Get the current terminal settings. */
	if (tcgetattr(the_pty.fd, &the_pty.term) < 0)
		return 1;
#endif

top:
	/*
	** Wait until at least one client is writable. Also wait on the control
	** socket in case a new client tries to connect.
	*/
	FD_ZERO(&readfds);
	FD_ZERO(&writefds);
	FD_SET(s, &readfds);
	highest_fd = s;
	for (p = clients, nclients = 0; p; p = p->next)
	{
		if (!p->attached)
			continue;
		FD_SET(p->fd, &writefds);
		if (p->fd > highest_fd)
			highest_fd = p->fd;
		nclients++;
	}
	if (nclients == 0)
		return 0;
	/* XXX In what cases should this be fatal... */
	if (select(highest_fd + 1, &readfds, &writefds, NULL, NULL) < 0)
		return 0;

	/* Send the data out to the clients. */
	for (p = clients, nclients = 0; p; p = p->next)
	{
		ssize_t written;

		if (!FD_ISSET(p->fd, &writefds))
			continue;

		written = 0;
		while (written < len)
		{
			ssize_t n = write(p->fd, buf + written, len - written);

			if (n > 0)
			{
				written += n;
				continue;
			}
			else if (n < 0 && errno == EINTR)
				continue;
			else if (n < 0 && errno != EAGAIN)
				nclients = -1;
			break;
		}
		if (nclients != -1 && written == len)
			nclients++;
	}

	/* Try again if nothing happened. */
	if (!FD_ISSET(s, &readfds) && nclients == 0)
		goto top;
	return 0;
}

/* Process activity on the control socket */
static void
control_activity(int s)
{
	int fd;
	struct client *p;
 
	/* Accept the new client and link it in. */
	fd = accept(s, NULL, NULL);
	if (fd < 0)
		return;
	else if (setnonblocking(fd) < 0)
	{
		close(fd);
		return;
	}

	/* Link it in. */
	p = malloc(sizeof(struct client));
	p->fd = fd;
	p->attached = 0;
	p->pprev = &clients;
	p->next = *(p->pprev);
	if (p->next)
		p->next->pprev = &p->next;
	*(p->pprev) = p;
}

/* Process activity from a client. */
static void
client_activity(struct client *p)
{
	ssize_t len;
	struct packet pkt;

	/* Read the activity. */
	len = read(p->fd, &pkt, sizeof(struct packet));
	if (len < 0 && (errno == EAGAIN || errno == EINTR))
		return;

	/* Close the client on an error. */
	if (len <= 0)
	{
		close(p->fd);
		if (p->next)
			p->next->pprev = p->pprev;
		*(p->pprev) = p->next;
		free(p);
		return;
	} 

	/* Push out data to the program. */
	if (pkt.type == MSG_PUSH)
	{
		if (pkt.len <= sizeof(pkt.u.buf))
			write(the_pty.fd, pkt.u.buf, pkt.len);
	}

	/* Attach or detach from the program. */
	else if (pkt.type == MSG_ATTACH)
		p->attached = 1;
	else if (pkt.type == MSG_DETACH)
		p->attached = 0;

	/* Window size change request, without a forced redraw. */
	else if (pkt.type == MSG_WINCH)
	{
		the_pty.ws = pkt.u.ws;
		ioctl(the_pty.fd, TIOCSWINSZ, &the_pty.ws);
	}

	/* Force a redraw using a particular method. */
	else if (pkt.type == MSG_REDRAW)
	{
		int method = pkt.len;

		/* If the client didn't specify a particular method, use
		** whatever we had on startup. */
		if (method == REDRAW_UNSPEC)
			method = redraw_method;
		if (method == REDRAW_NONE)
			return;

		/* Set the window size. */
		the_pty.ws = pkt.u.ws;
		ioctl(the_pty.fd, TIOCSWINSZ, &the_pty.ws);

		/* Send a ^L character if the terminal is in no-echo and
		** character-at-a-time mode. */
		if (method == REDRAW_CTRL_L)
		{
			char c = '\f';

                	if (((the_pty.term.c_lflag & (ECHO|ICANON)) == 0) &&
                        	(the_pty.term.c_cc[VMIN] == 1))
			{
				write(the_pty.fd, &c, 1);
			}
		}
		/* Send a WINCH signal to the program. */
		else if (method == REDRAW_WINCH)
		{
			killpty(&the_pty, SIGWINCH);
		}
	}
}

/* The master process - It watches over the pty process and the attached */
/* clients. */
static int
master_process(int s, char **argv, int waitattach, int statusfd)
{
	struct client *p, *next;
	fd_set readfds;
	int highest_fd;
	int nullfd;

	/* Okay, disassociate ourselves from the original terminal, as we
	** don't care what happens to it. */
	setsid();

	/* Set a trap to unlink the socket when we die. */
	atexit(unlink_socket);

	/* Set up some signals. */
	struct sigaction sa;
	sa.sa_flags = SA_NOCLDSTOP;
	sigemptyset(&sa.sa_mask);

	sa.sa_handler = SIG_IGN;
	sigaction(SIGPIPE, &sa, NULL);
	sigaction(SIGXFSZ, &sa, NULL);
	sigaction(SIGHUP, &sa, NULL);
	sigaction(SIGTTIN, &sa, NULL);
	sigaction(SIGTTOU, &sa, NULL);

	sa.sa_handler = die;
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGCHLD, &sa, NULL);


	/* Create a pty in which the process is running. */
	if (init_pty(argv, statusfd) < 0)
	{
		if (statusfd != -1)
			dup2(statusfd, 1);
		fprintf(stderr, "%s: init_pty: %s\n", progname,
			strerror(errno));
		return 1;
	}
	
	/* Close statusfd, since we don't need it anymore. */
	if (statusfd != -1)
		close(statusfd);

	/* Make sure stdin/stdout/stderr point to /dev/null. We are now a
	** daemon. */
	nullfd = open("/dev/null", O_RDWR);
	dup2(nullfd, 0);
	dup2(nullfd, 1);
	dup2(nullfd, 2);
	if (nullfd > 2)
		close(nullfd);

	/* Main loop. */
	stop = 0;
	while (!stop)
	{
		/* Re-initialize the file descriptor set for select. */
		FD_ZERO(&readfds);
		FD_SET(s, &readfds);
		highest_fd = s;

		/*
		** When waitattach is set, wait until the client attaches
		** before trying to read from the pty.
		*/
		if (waitattach)
		{
			if (clients && clients->attached)
				waitattach = 0;
		}
		else
		{
			FD_SET(the_pty.fd, &readfds);
			if (the_pty.fd > highest_fd)
				highest_fd = the_pty.fd;
		}

		for (p = clients; p; p = p->next)
		{
			FD_SET(p->fd, &readfds);
			if (p->fd > highest_fd)
				highest_fd = p->fd;
		}

		/* Wait for something to happen. */
		if (select(highest_fd + 1, &readfds, NULL, NULL, NULL) < 0)
		{
			if (errno == EINTR || errno == EAGAIN)
				continue;
			return 1;
		}

		/* New client? */
		if (FD_ISSET(s, &readfds))
			control_activity(s);
		/* Activity on a client? */
		for (p = clients; p; p = next)
		{
			next = p->next;
			if (FD_ISSET(p->fd, &readfds))
				client_activity(p);
		}
		/* pty activity? */
		if (FD_ISSET(the_pty.fd, &readfds))
			if (pty_activity(s))
				return 1;
	}
	return 0;
}

static int
master_main(char **argv, int waitattach, int nofork)
{
	int fd[2] = {-1, -1};
	int s;
	pid_t pid;

	/* Use a default redraw method if one hasn't been specified yet. */
	if (redraw_method == REDRAW_UNSPEC)
		redraw_method = REDRAW_CTRL_L;

	/* Create the unix domain socket. */
	s = create_socket(sockname);
	if (s < 0)
	{
		fprintf(stderr, "%s: %s: %s\n", progname, sockname,
			strerror(errno));
		return 1;
	}

#if defined(F_SETFD) && defined(FD_CLOEXEC)
	fcntl(s, F_SETFD, FD_CLOEXEC);

	/* If FD_CLOEXEC works, create a pipe and use it to report any errors
	** that occur while trying to execute the program. */
	if (!nofork && pipe(fd) >= 0)
	{
		if (fcntl(fd[0], F_SETFD, FD_CLOEXEC) < 0 ||
		    fcntl(fd[1], F_SETFD, FD_CLOEXEC) < 0)
		{
			close(fd[0]);
			close(fd[1]);
			fd[0] = fd[1] = -1;
		}
	}
#endif

	if (!nofork)
	{
		/* Fork off so we can daemonize and such */
		pid = fork();
		if (pid < 0)
		{
			fprintf(stderr, "%s: fork: %s\n", progname,
				strerror(errno));
			unlink_socket();
			return 1;
		}
		else if (pid > 0)
		{
			/* Parent - just return. */
#if defined(F_SETFD) && defined(FD_CLOEXEC)
			/* Check if an error occurred while trying to execute
			** the program. */
			if (fd[0] != -1)
			{
				char buf[1024];
				ssize_t len;

				close(fd[1]);
				len = read(fd[0], buf, sizeof(buf));
				if (len > 0)
				{
					write(2, buf, len);
					kill(pid, SIGTERM);
					return 1;
				}
				close(fd[0]);
			}
#endif
			close(s);
			return 0;
		}
		/* Child - this becomes the master */
		if (fd[0] != -1)
			close(fd[0]);
	}
	else /* Not forking, send status to stderr */
		fd[1] = dup(2);

	return master_process(s, argv, waitattach, fd[1]);
}

int
main(int argc, char **argv)
{
	int nofork = 0;
	int waitattach = 0;

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
			if (*p == 'n')
				nofork = 1;
			else if (*p == 'w')
				waitattach = 1;
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

	if (argc < 1)
	{
		fprintf(stderr, "%s: No command was specified.\n", progname);
		fprintf(stderr, "Try '%s --help' for more information.\n",
			progname);
		return 1;
	}

	/* Save the original terminal settings. */
	if (tcgetattr(0, &orig_term) < 0)
	{
		memset(&orig_term, 0, sizeof(struct termios));
		dont_have_tty = 1;
	}

	return master_main(argv, waitattach, nofork);
}

/* BSDish functions for systems that don't have them. */
#ifndef HAVE_OPENPTY
#define HAVE_OPENPTY
/* openpty: Use /dev/ptmx and Unix98 if we have it. */
#if defined(HAVE_PTSNAME) && defined(HAVE_GRANTPT) && defined(HAVE_UNLOCKPT)
int
openpty(int *amaster, int *aslave, char *name, struct termios *termp,
	struct winsize *winp)
{
	int master, slave;
	char *buf;

#ifdef _AIX
	master = open("/dev/ptc", O_RDWR|O_NOCTTY);
	if (master < 0)
		return -1;
	buf = ttyname(master);
	if (!buf)
		return -1;

	slave = open(buf, O_RDWR|O_NOCTTY);
	if (slave < 0)
		return -1;
#else
	master = open("/dev/ptmx", O_RDWR);
	if (master < 0)
		return -1;
	if (grantpt(master) < 0)
		return -1;
	if (unlockpt(master) < 0)
		return -1;
	buf = ptsname(master);
	if (!buf)
		return -1;

	slave = open(buf, O_RDWR|O_NOCTTY);
	if (slave < 0)
		return -1;

#ifdef I_PUSH
	if (ioctl(slave, I_PUSH, "ptem") < 0)
		return -1;
	if (ioctl(slave, I_PUSH, "ldterm") < 0)
		return -1;
#endif
#endif

	*amaster = master;
	*aslave = slave;
	if (name)
		strcpy(name, buf);
	if (termp)
		tcsetattr(slave, TCSAFLUSH, termp);
	if (winp)
		ioctl(slave, TIOCSWINSZ, winp);
	return 0;
}
#else
#error Do not know how to define openpty.
#endif
#endif

#ifndef HAVE_FORKPTY
#if defined(HAVE_OPENPTY)
pid_t
forkpty(int *amaster, char *name, struct termios *termp,
	struct winsize *winp)
{
	pid_t pid;
	int master, slave;

	if (openpty(&master, &slave, name, termp, winp) < 0)
		return -1;
	*amaster = master;

	/* Fork off... */
	pid = fork();
	if (pid < 0)
		return -1;
	else if (pid == 0)
	{
		char *buf;
		int fd;

		setsid();
#ifdef TIOCSCTTY
		buf = NULL;
		if (ioctl(slave, TIOCSCTTY, NULL) < 0)
			_exit(1);
#elif defined(_AIX)
		fd = open("/dev/tty", O_RDWR|O_NOCTTY);
		if (fd >= 0)
		{
			ioctl(fd, TIOCNOTTY, NULL);
			close(fd);
		}

		buf = ttyname(master);
		fd = open(buf, O_RDWR);
		close(fd);

		fd = open("/dev/tty", O_WRONLY);
		if (fd < 0)
			_exit(1);
		close(fd);

		if (termp && tcsetattr(slave, TCSAFLUSH, termp) == -1)
			_exit(1);
		if (ioctl(slave, TIOCSWINSZ, winp) == -1)
			_exit(1);
#else
		buf = ptsname(master);
		fd = open(buf, O_RDWR);
		close(fd);
#endif
		dup2(slave, 0);
		dup2(slave, 1);
		dup2(slave, 2);

		if (slave > 2)
			close(slave);
		close(master);
		return 0;
	}
	else
	{
		close(slave);
		return pid;
	}
}
#else
#error Do not know how to define forkpty.
#endif
#endif
