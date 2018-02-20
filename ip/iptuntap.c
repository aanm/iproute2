/*
 * iptunnel.c	       "ip tuntap"
 *
 *		This program is free software; you can redistribute it and/or
 *		modify it under the terms of the GNU General Public License
 *		as published by the Free Software Foundation; either version
 *		2 of the License, or (at your option) any later version.
 *
 * Authors:	David Woodhouse <David.Woodhouse@intel.com>
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <sys/ioctl.h>
#include <linux/if.h>
#include <linux/if_tun.h>
#include <linux/if_arp.h>
#include <pwd.h>
#include <grp.h>
#include <fcntl.h>
#include <dirent.h>
#include <errno.h>
#include <glob.h>

#include "rt_names.h"
#include "utils.h"
#include "ip_common.h"

static const char drv_name[] = "tun";

#define TUNDEV "/dev/net/tun"

static void usage(void) __attribute__((noreturn));

static void usage(void)
{
	fprintf(stderr, "Usage: ip tuntap { add | del | show | list | lst | help } [ dev PHYS_DEV ]\n");
	fprintf(stderr, "          [ mode { tun | tap } ] [ user USER ] [ group GROUP ]\n");
	fprintf(stderr, "          [ one_queue ] [ pi ] [ vnet_hdr ] [ multi_queue ] [ name NAME ]\n");
	fprintf(stderr, "\n");
	fprintf(stderr, "Where: USER  := { STRING | NUMBER }\n");
	fprintf(stderr, "       GROUP := { STRING | NUMBER }\n");
	exit(-1);
}

static int tap_add_ioctl(struct ifreq *ifr, uid_t uid, gid_t gid)
{
	int fd;
	int ret = -1;

#ifdef IFF_TUN_EXCL
	ifr->ifr_flags |= IFF_TUN_EXCL;
#endif

	fd = open(TUNDEV, O_RDWR);
	if (fd < 0) {
		perror("open");
		return -1;
	}
	if (ioctl(fd, TUNSETIFF, ifr)) {
		perror("ioctl(TUNSETIFF)");
		goto out;
	}
	if (uid != -1 && ioctl(fd, TUNSETOWNER, uid)) {
		perror("ioctl(TUNSETOWNER)");
		goto out;
	}
	if (gid != -1 && ioctl(fd, TUNSETGROUP, gid)) {
		perror("ioctl(TUNSETGROUP)");
		goto out;
	}
	if (ioctl(fd, TUNSETPERSIST, 1)) {
		perror("ioctl(TUNSETPERSIST)");
		goto out;
	}
	ret = 0;
 out:
	close(fd);
	return ret;
}

static int tap_del_ioctl(struct ifreq *ifr)
{
	int fd = open(TUNDEV, O_RDWR);
	int ret = -1;

	if (fd < 0) {
		perror("open");
		return -1;
	}
	if (ioctl(fd, TUNSETIFF, ifr)) {
		perror("ioctl(TUNSETIFF)");
		goto out;
	}
	if (ioctl(fd, TUNSETPERSIST, 0)) {
		perror("ioctl(TUNSETPERSIST)");
		goto out;
	}
	ret = 0;
 out:
	close(fd);
	return ret;

}
static int parse_args(int argc, char **argv,
		      struct ifreq *ifr, uid_t *uid, gid_t *gid)
{
	int count = 0;

	memset(ifr, 0, sizeof(*ifr));

	ifr->ifr_flags |= IFF_NO_PI;

	while (argc > 0) {
		if (matches(*argv, "mode") == 0) {
			NEXT_ARG();
			if (matches(*argv, "tun") == 0) {
				if (ifr->ifr_flags & IFF_TAP) {
					fprintf(stderr, "You managed to ask for more than one tunnel mode.\n");
					exit(-1);
				}
				ifr->ifr_flags |= IFF_TUN;
			} else if (matches(*argv, "tap") == 0) {
				if (ifr->ifr_flags & IFF_TUN) {
					fprintf(stderr, "You managed to ask for more than one tunnel mode.\n");
					exit(-1);
				}
				ifr->ifr_flags |= IFF_TAP;
			} else {
				fprintf(stderr, "Unknown tunnel mode \"%s\"\n", *argv);
				exit(-1);
			}
		} else if (uid && matches(*argv, "user") == 0) {
			char *end;
			unsigned long user;

			NEXT_ARG();
			if (**argv && ((user = strtol(*argv, &end, 10)), !*end))
				*uid = user;
			else {
				struct passwd *pw = getpwnam(*argv);

				if (!pw) {
					fprintf(stderr, "invalid user \"%s\"\n", *argv);
					exit(-1);
				}
				*uid = pw->pw_uid;
			}
		} else if (gid && matches(*argv, "group") == 0) {
			char *end;
			unsigned long group;

			NEXT_ARG();

			if (**argv && ((group = strtol(*argv, &end, 10)), !*end))
				*gid = group;
			else {
				struct group *gr = getgrnam(*argv);

				if (!gr) {
					fprintf(stderr, "invalid group \"%s\"\n", *argv);
					exit(-1);
				}
				*gid = gr->gr_gid;
			}
		} else if (matches(*argv, "pi") == 0) {
			ifr->ifr_flags &= ~IFF_NO_PI;
		} else if (matches(*argv, "one_queue") == 0) {
			ifr->ifr_flags |= IFF_ONE_QUEUE;
		} else if (matches(*argv, "vnet_hdr") == 0) {
			ifr->ifr_flags |= IFF_VNET_HDR;
		} else if (matches(*argv, "multi_queue") == 0) {
			ifr->ifr_flags |= IFF_MULTI_QUEUE;
		} else if (matches(*argv, "dev") == 0) {
			NEXT_ARG();
			if (get_ifname(ifr->ifr_name, *argv))
				invarg("\"dev\" not a valid ifname", *argv);
		} else {
			if (matches(*argv, "name") == 0) {
				NEXT_ARG();
			} else if (matches(*argv, "help") == 0)
				usage();
			if (ifr->ifr_name[0])
				duparg2("name", *argv);
			if (get_ifname(ifr->ifr_name, *argv))
				invarg("\"name\" not a valid ifname", *argv);
		}
		count++;
		argc--; argv++;
	}

	if (!(ifr->ifr_flags & TUN_TYPE_MASK)) {
		fprintf(stderr, "You failed to specify a tunnel mode\n");
		return -1;
	}

	return 0;
}


static int do_add(int argc, char **argv)
{
	struct ifreq ifr;
	uid_t uid = -1;
	gid_t gid = -1;

	if (parse_args(argc, argv, &ifr, &uid, &gid) < 0)
		return -1;

	return tap_add_ioctl(&ifr, uid, gid);
}

static int do_del(int argc, char **argv)
{
	struct ifreq ifr;

	if (parse_args(argc, argv, &ifr, NULL, NULL) < 0)
		return -1;

	return tap_del_ioctl(&ifr);
}

static void print_flags(long flags)
{
	if (flags & IFF_TUN)
		printf(" tun");

	if (flags & IFF_TAP)
		printf(" tap");

	if (!(flags & IFF_NO_PI))
		printf(" pi");

	if (flags & IFF_ONE_QUEUE)
		printf(" one_queue");

	if (flags & IFF_VNET_HDR)
		printf(" vnet_hdr");

	flags &= ~(IFF_TUN|IFF_TAP|IFF_NO_PI|IFF_ONE_QUEUE|IFF_VNET_HDR);
	if (flags)
		printf(" UNKNOWN_FLAGS:%lx", flags);
}

static char *pid_name(pid_t pid)
{
	char *comm;
	FILE *f;
	int err;

	err = asprintf(&comm, "/proc/%d/comm", pid);
	if (err < 0)
		return NULL;

	f = fopen(comm, "r");
	free(comm);
	if (!f) {
		perror("fopen");
		return NULL;
	}

	if (fscanf(f, "%ms\n", &comm) != 1) {
		perror("fscanf");
		comm = NULL;
	}


	if (fclose(f))
		perror("fclose");

	return comm;
}

static void show_processes(const char *name)
{
	glob_t globbuf = { };
	char **fd_path;
	int err;

	err = glob("/proc/[0-9]*/fd/[0-9]*", GLOB_NOSORT,
		   NULL, &globbuf);
	if (err)
		return;

	fd_path = globbuf.gl_pathv;
	while (*fd_path) {
		const char *dev_net_tun = "/dev/net/tun";
		const size_t linkbuf_len = strlen(dev_net_tun) + 2;
		char linkbuf[linkbuf_len], *fdinfo;
		int pid, fd;
		FILE *f;

		if (sscanf(*fd_path, "/proc/%d/fd/%d", &pid, &fd) != 2)
			goto next;

		if (pid == getpid())
			goto next;

		err = readlink(*fd_path, linkbuf, linkbuf_len - 1);
		if (err < 0) {
			perror("readlink");
			goto next;
		}
		linkbuf[err] = '\0';
		if (strcmp(dev_net_tun, linkbuf))
			goto next;

		if (asprintf(&fdinfo, "/proc/%d/fdinfo/%d", pid, fd) < 0)
			goto next;

		f = fopen(fdinfo, "r");
		free(fdinfo);
		if (!f) {
			perror("fopen");
			goto next;
		}

		while (!feof(f)) {
			char *key = NULL, *value = NULL;

			err = fscanf(f, "%m[^:]: %ms\n", &key, &value);
			if (err == EOF) {
				if (ferror(f))
					perror("fscanf");
				break;
			} else if (err == 2 &&
				   !strcmp("iff", key) &&
				   !strcmp(name, value)) {
				char *pname = pid_name(pid);

				printf(" %s(%d)", pname ? : "<NULL>", pid);
				free(pname);
			}

			free(key);
			free(value);
		}
		if (fclose(f))
			perror("fclose");

next:
		++fd_path;
	}

	globfree(&globbuf);
}

static int tuntap_filter_req(struct nlmsghdr *nlh, int reqlen)
{
	struct rtattr *linkinfo;
	int err;

	linkinfo = addattr_nest(nlh, reqlen, IFLA_LINKINFO);

	err = addattr_l(nlh, reqlen, IFLA_INFO_KIND,
			drv_name, sizeof(drv_name) - 1);
	if (err)
		return err;

	addattr_nest_end(nlh, linkinfo);

	return 0;
}

static int print_tuntap(const struct sockaddr_nl *who,
			struct nlmsghdr *n, void *arg)
{
	struct ifinfomsg *ifi = NLMSG_DATA(n);
	struct rtattr *tb[IFLA_MAX+1];
	struct rtattr *linkinfo[IFLA_INFO_MAX+1];
	const char *name, *kind;
	long flags, owner = -1, group = -1;

	if (n->nlmsg_type != RTM_NEWLINK && n->nlmsg_type != RTM_DELLINK)
		return 0;

	if (n->nlmsg_len < NLMSG_LENGTH(sizeof(*ifi)))
		return -1;

	switch (ifi->ifi_type) {
	case ARPHRD_NONE:
	case ARPHRD_ETHER:
		break;
	default:
		return 0;
	}

	parse_rtattr(tb, IFLA_MAX, IFLA_RTA(ifi), IFLA_PAYLOAD(n));

	if (!tb[IFLA_IFNAME])
		return 0;

	if (!tb[IFLA_LINKINFO])
		return 0;

	parse_rtattr_nested(linkinfo, IFLA_INFO_MAX, tb[IFLA_LINKINFO]);

	if (!linkinfo[IFLA_INFO_KIND])
		return 0;

	kind = rta_getattr_str(linkinfo[IFLA_INFO_KIND]);
	if (strcmp(kind, drv_name))
		return 0;

	name = rta_getattr_str(tb[IFLA_IFNAME]);

	if (read_prop(name, "tun_flags", &flags))
		return 0;
	if (read_prop(name, "owner", &owner))
		return 0;
	if (read_prop(name, "group", &group))
		return 0;

	printf("%s:", name);
	print_flags(flags);
	if (owner != -1)
		printf(" user %ld", owner);
	if (group != -1)
		printf(" group %ld", group);
	fputc('\n', stdout);
	if (show_details) {
		printf("\tAttached to processes:");
		show_processes(name);
		fputc('\n', stdout);
	}

	return 0;
}

static int do_show(int argc, char **argv)
{
	if (rtnl_wilddump_req_filter_fn(&rth, AF_UNSPEC, RTM_GETLINK,
					tuntap_filter_req) < 0) {
		perror("Cannot send dump request\n");
		return -1;
	}

	if (rtnl_dump_filter(&rth, print_tuntap, NULL) < 0) {
		fprintf(stderr, "Dump terminated\n");
		return -1;
	}

	return 0;
}

int do_iptuntap(int argc, char **argv)
{
	if (argc > 0) {
		if (matches(*argv, "add") == 0)
			return do_add(argc-1, argv+1);
		if (matches(*argv, "delete") == 0)
			return do_del(argc-1, argv+1);
		if (matches(*argv, "show") == 0 ||
		    matches(*argv, "lst") == 0 ||
		    matches(*argv, "list") == 0)
			return do_show(argc-1, argv+1);
		if (matches(*argv, "help") == 0)
			usage();
	} else
		return do_show(0, NULL);

	fprintf(stderr, "Command \"%s\" is unknown, try \"ip tuntap help\".\n",
		*argv);
	exit(-1);
}

static void print_owner(FILE *f, uid_t uid)
{
	struct passwd *pw = getpwuid(uid);

	if (pw)
		fprintf(f, "user %s ", pw->pw_name);
	else
		fprintf(f, "user %u ", uid);
}

static void print_group(FILE *f, gid_t gid)
{
	struct group *group = getgrgid(gid);

	if (group)
		fprintf(f, "group %s ", group->gr_name);
	else
		fprintf(f, "group %u ", gid);
}

static void print_mq(FILE *f, struct rtattr *tb[])
{
	if (!tb[IFLA_TUN_MULTI_QUEUE] ||
	    !rta_getattr_u8(tb[IFLA_TUN_MULTI_QUEUE]))
		return;

	fprintf(f, "multi_queue ");

	if (tb[IFLA_TUN_NUM_QUEUES]) {
		fprintf(f, "numqueues %u ",
			rta_getattr_u32(tb[IFLA_TUN_NUM_QUEUES]));
	}

	if (tb[IFLA_TUN_NUM_DISABLED_QUEUES]) {
		fprintf(f, "numdisabled %u ",
			rta_getattr_u32(tb[IFLA_TUN_NUM_DISABLED_QUEUES]));
	}
}

static void print_onoff(FILE *f, const char *flag, __u8 val)
{
	fprintf(f, "%s %s ", flag, val ? "on" : "off");
}

static void tun_print_opt(struct link_util *lu, FILE *f, struct rtattr *tb[])
{
	if (!tb)
		return;

	if (tb[IFLA_TUN_TYPE]) {
		__u8 type = rta_getattr_u8(tb[IFLA_TUN_TYPE]);

		if (type == IFF_TUN)
			fprintf(f, "type tun ");
		else if (type == IFF_TAP)
			fprintf(f, "type tap ");
		else
			fprintf(f, "type UNKNOWN:%hhu ", type);
	}

	if (tb[IFLA_TUN_PI])
		print_onoff(f, "pi", rta_getattr_u8(tb[IFLA_TUN_PI]));

	if (tb[IFLA_TUN_VNET_HDR]) {
		print_onoff(f, "vnet_hdr",
			    rta_getattr_u8(tb[IFLA_TUN_VNET_HDR]));
	}

	print_mq(f, tb);

	if (tb[IFLA_TUN_PERSIST])
		print_onoff(f, "persist", rta_getattr_u8(tb[IFLA_TUN_PERSIST]));

	if (tb[IFLA_TUN_OWNER])
		print_owner(f, rta_getattr_u32(tb[IFLA_TUN_OWNER]));

	if (tb[IFLA_TUN_GROUP])
		print_group(f, rta_getattr_u32(tb[IFLA_TUN_GROUP]));
}

struct link_util tun_link_util = {
	.id = "tun",
	.maxattr = IFLA_TUN_MAX,
	.print_opt = tun_print_opt,
};
