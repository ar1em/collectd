/**
 * collectd - src/traffic.c
 * Copyright (C) 2005,2006  Florian octo Forster
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin St, Fifth Floor, Boston, MA  02110-1301 USA
 *
 * Authors:
 *   Florian octo Forster <octo at verplant.org>
 *   Sune Marcher <sm at flork.dk>
 **/

#include "collectd.h"
#include "common.h"
#include "plugin.h"
#include "configfile.h"

#if HAVE_SYS_TYPES_H
#  include <sys/types.h>
#endif
#if HAVE_SYS_SOCKET_H
#  include <sys/socket.h>
#endif

/* One cannot include both. This sucks. */
#if HAVE_LINUX_IF_H
#  include <linux/if.h>
#elif HAVE_NET_IF_H
#  include <net/if.h>
#endif

#if HAVE_LINUX_NETDEVICE_H
#  include <linux/netdevice.h>
#endif
#if HAVE_IFADDRS_H
#  include <ifaddrs.h>
#endif

#define MODULE_NAME "traffic"

/*
 * Various people have reported problems with `getifaddrs' and varying versions
 * of `glibc'. That's why it's disabled by default. Since more statistics are
 * available this way one may enable it using the `--enable-getifaddrs' option
 * of the configure script. -octo
 */
#if KERNEL_LINUX
# if !COLLECT_GETIFADDRS
#  undef HAVE_GETIFADDRS
# endif /* !COLLECT_GETIFADDRS */
#endif /* KERNEL_LINUX */

#if HAVE_GETIFADDRS || KERNEL_LINUX || HAVE_LIBKSTAT || HAVE_LIBSTATGRAB
# define TRAFFIC_HAVE_READ 1
#else
# define TRAFFIC_HAVE_READ 0
#endif

#define BUFSIZE 512

/*
 * (Module-)Global variables
 */
/* 2^32 = 4294967296 = ~4.2GByte/s = ~34GBit/s */
static data_source_t octets_dsrc[2] =
{
	{"rx", DS_TYPE_COUNTER, 0, 4294967295.0},
	{"tx", DS_TYPE_COUNTER, 0, 4294967295.0}
};

static data_set_t octets_ds =
{
	"if_octets", 2, octets_dsrc
};

static data_source_t packets_dsrc[2] =
{
	{"rx", DS_TYPE_COUNTER, 0, 4294967295.0},
	{"tx", DS_TYPE_COUNTER, 0, 4294967295.0}
};

static data_set_t packets_ds =
{
	"if_packets", 2, packets_dsrc
};

static data_source_t errors_dsrc[2] =
{
	{"rx", DS_TYPE_COUNTER, 0, 4294967295.0},
	{"tx", DS_TYPE_COUNTER, 0, 4294967295.0}
};

static data_set_t errors_ds =
{
	"if_errors", 2, errors_dsrc
};

static const char *config_keys[] =
{
	"Interface",
	"IgnoreSelected",
	NULL
};
static int config_keys_num = 2;

static char **if_list = NULL;
static int    if_list_num = 0;
/* 
 * if_list_action:
 * 0 => default is to collect selected interface
 * 1 => ignore selcted interfaces
 */
static int    if_list_action = 0;

#ifdef HAVE_LIBKSTAT
#define MAX_NUMIF 256
extern kstat_ctl_t *kc;
static kstat_t *ksp[MAX_NUMIF];
static int numif = 0;
#endif /* HAVE_LIBKSTAT */

static int interface_config (const char *key, const char *value)
{
	char **temp;

	if (strcasecmp (key, "Interface") == 0)
	{
		temp = (char **) realloc (if_list, (if_list_num + 1) * sizeof (char *));
		if (temp == NULL)
		{
			syslog (LOG_EMERG, "Cannot allocate more memory.");
			return (1);
		}
		if_list = temp;

		if ((if_list[if_list_num] = strdup (value)) == NULL)
		{
			syslog (LOG_EMERG, "Cannot allocate memory.");
			return (1);
		}
		if_list_num++;
	}
	else if (strcasecmp (key, "IgnoreSelected") == 0)
	{
		if ((strcasecmp (value, "True") == 0)
				|| (strcasecmp (value, "Yes") == 0)
				|| (strcasecmp (value, "On") == 0))
			if_list_action = 1;
		else
			if_list_action = 0;
	}
	else
	{
		return (-1);
	}

	return (0);
}

#if HAVE_LIBKSTAT
static int traffic_init (void)
{
#if HAVE_LIBKSTAT
	kstat_t *ksp_chain;
	unsigned long long val;

	numif = 0;

	if (kc == NULL)
		return (-1);

	for (numif = 0, ksp_chain = kc->kc_chain;
			(numif < MAX_NUMIF) && (ksp_chain != NULL);
			ksp_chain = ksp_chain->ks_next)
	{
		if (strncmp (ksp_chain->ks_class, "net", 3))
			continue;
		if (ksp_chain->ks_type != KSTAT_TYPE_NAMED)
			continue;
		if (kstat_read (kc, ksp_chain, NULL) == -1)
			continue;
		if ((val = get_kstat_value (ksp_chain, "obytes")) == -1LL)
			continue;
		ksp[numif++] = ksp_chain;
	}
#endif /* HAVE_LIBKSTAT */

	return (0);
} /* int traffic_init */
#endif /* HAVE_LIBKSTAT */

/*
 * Check if this interface/instance should be ignored. This is called from
 * both, `submit' and `write' to give client and server the ability to
 * ignore certain stuff..
 */
static int check_ignore_if (const char *interface)
{
	int i;

	/* If no interfaces are given collect all interfaces. Mostly to be
	 * backwards compatible, but also because this is much easier. */
	if (if_list_num < 1)
		return (0);

	for (i = 0; i < if_list_num; i++)
		if (strcasecmp (interface, if_list[i]) == 0)
			return (if_list_action);
	return (1 - if_list_action);
} /* int check_ignore_if */

#if TRAFFIC_HAVE_READ
static void if_submit (const char *dev, const char *type,
		unsigned long long rx,
		unsigned long long tx)
{
	value_t values[2];
	value_list_t vl;

	if (check_ignore_if (dev))
		return;

	values[0].counter = rx;
	values[1].counter = tx;

	vl.values = values;
	vl.values_len = 2;
	vl.time = time (NULL);

	/* FIXME: do this globally */
	if (gethostname (vl.host, sizeof (vl.host)) != 0)
	{
		syslog (LOG_ERR, "load plugin: gethostname failed: %s",
				strerror (errno));
		return;
	}
	strcpy (vl.plugin, "interface");
	strcpy (vl.plugin_instance, "");
	strncpy (vl.type_instance, dev, sizeof (vl.type_instance));

	plugin_dispatch_values (type, &vl);
} /* void if_submit */

static int traffic_read (void)
{
#if HAVE_GETIFADDRS
	struct ifaddrs *if_list;
	struct ifaddrs *if_ptr;

/* Darin/Mac OS X and possible other *BSDs */
#if HAVE_STRUCT_IF_DATA
#  define IFA_DATA if_data
#  define IFA_RX_BYTES ifi_ibytes
#  define IFA_TX_BYTES ifi_obytes
#  define IFA_RX_PACKT ifi_ipackets
#  define IFA_TX_PACKT ifi_opackets
#  define IFA_RX_ERROR ifi_ierrors
#  define IFA_TX_ERROR ifi_oerrors
/* #endif HAVE_STRUCT_IF_DATA */

#elif HAVE_STRUCT_NET_DEVICE_STATS
#  define IFA_DATA net_device_stats
#  define IFA_RX_BYTES rx_bytes
#  define IFA_TX_BYTES tx_bytes
#  define IFA_RX_PACKT rx_packets
#  define IFA_TX_PACKT tx_packets
#  define IFA_RX_ERROR rx_errors
#  define IFA_TX_ERROR tx_errors
#else
#  error "No suitable type for `struct ifaddrs->ifa_data' found."
#endif

	struct IFA_DATA *if_data;

	if (getifaddrs (&if_list) != 0)
		return;

	for (if_ptr = if_list; if_ptr != NULL; if_ptr = if_ptr->ifa_next)
	{
		if ((if_data = (struct IFA_DATA *) if_ptr->ifa_data) == NULL)
			continue;

		if_submit (if_ptr->ifa_name, "if_octets",
				if_data->IFA_RX_BYTES,
				if_data->IFA_TX_BYTES);
		if_submit (if_ptr->ifa_name, "if_packets",
				if_data->IFA_RX_PACKT,
				if_data->IFA_TX_PACKT);
		if_submit (if_ptr->ifa_name, "if_errors",
				if_data->IFA_RX_ERROR,
				if_data->IFA_TX_ERROR);
	}

	freeifaddrs (if_list);
/* #endif HAVE_GETIFADDRS */

#elif KERNEL_LINUX
	FILE *fh;
	char buffer[1024];
	unsigned long long incoming, outgoing;
	char *device;
	
	char *dummy;
	char *fields[16];
	int numfields;

	if ((fh = fopen ("/proc/net/dev", "r")) == NULL)
	{
		syslog (LOG_WARNING, "traffic: fopen: %s", strerror (errno));
		return (-1);
	}

	while (fgets (buffer, 1024, fh) != NULL)
	{
		if (!(dummy = strchr(buffer, ':')))
			continue;
		dummy[0] = '\0';
		dummy++;

		device = buffer;
		while (device[0] == ' ')
			device++;

		if (device[0] == '\0')
			continue;
		
		numfields = strsplit (dummy, fields, 16);

		if (numfields < 11)
			continue;

		incoming = atoll (fields[0]);
		outgoing = atoll (fields[8]);
		if_submit (device, "if_octets", incoming, outgoing);

		incoming = atoll (fields[1]);
		outgoing = atoll (fields[9]);
		if_submit (device, "if_packets", incoming, outgoing);

		incoming = atoll (fields[2]);
		outgoing = atoll (fields[10]);
		if_submit (device, "if_errors", incoming, outgoing);
	}

	fclose (fh);
/* #endif KERNEL_LINUX */

#elif HAVE_LIBKSTAT
	int i;
	unsigned long long rx;
	unsigned long long tx;

	if (kc == NULL)
		return;

	for (i = 0; i < numif; i++)
	{
		if (kstat_read (kc, ksp[i], NULL) == -1)
			continue;

		rx = get_kstat_value (ksp[i], "rbytes");
		tx = get_kstat_value (ksp[i], "obytes");
		if ((rx != -1LL) || (tx != -1LL))
			if_submit (ksp[i]->ks_name, "if_octets", rx, tx);

		rx = get_kstat_value (ksp[i], "ipackets");
		tx = get_kstat_value (ksp[i], "opackets");
		if ((rx != -1LL) || (tx != -1LL))
			if_submit (ksp[i]->ks_name, "if_packets", rx, tx);

		rx = get_kstat_value (ksp[i], "ierrors");
		tx = get_kstat_value (ksp[i], "oerrors");
		if ((rx != -1LL) || (tx != -1LL))
			if_submit (ksp[i]->ks_name, "if_errors", rx, tx);
	}
/* #endif HAVE_LIBKSTAT */

#elif defined(HAVE_LIBSTATGRAB)
	sg_network_io_stats *ios;
	int i, num;

	ios = sg_get_network_io_stats (&num);

	for (i = 0; i < num; i++)
		if_submit (ios[i].interface_name, "if_octets", ios[i].rx, ios[i].tx);
#endif /* HAVE_LIBSTATGRAB */

	return (0);
} /* int traffic_read */
#endif /* TRAFFIC_HAVE_READ */

void module_register (void)
{
	plugin_register_data_set (&octets_ds);
	plugin_register_data_set (&packets_ds);
	plugin_register_data_set (&errors_ds);

	plugin_register_config ("interface", interface_config,
			config_keys, config_keys_num);

#if HAVE_LIBKSTAT
	plugin_register_init ("interface", traffic_init);
#endif

#if TRAFFIC_HAVE_READ
	plugin_register_read ("interface", traffic_read);
#endif
}

#undef BUFSIZE
#undef MODULE_NAME
