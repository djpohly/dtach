#include "dtach.h"

void
init_sockaddr_un(struct sockaddr_un *sockun, char *name)
{
	sockun->sun_family = AF_UNIX;
	size_t name_bytes = strlen(name);
	size_t sun_path_bytes = sizeof(sockun->sun_path) - 1;
	if (name_bytes > sun_path_bytes)
	{
		printf("%s: %s: File name too long (%zu > %zu bytes)\n",
			progname, name, name_bytes, sun_path_bytes);
		exit(1);
	}
	strcpy(sockun->sun_path, name);
}
