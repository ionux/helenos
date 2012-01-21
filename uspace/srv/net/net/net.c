/*
 * Copyright (c) 2009 Lukas Mejdrech
 * Copyright (c) 2011 Radim Vansa
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * - Redistributions of source code must retain the above copyright
 *   notice, this list of conditions and the following disclaimer.
 * - Redistributions in binary form must reproduce the above copyright
 *   notice, this list of conditions and the following disclaimer in the
 *   documentation and/or other materials provided with the distribution.
 * - The name of the author may not be used to endorse or promote products
 *   derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/** @addtogroup net
 * @{
 */

#include <assert.h>
#include <async.h>
#include <ctype.h>
#include <ddi.h>
#include <errno.h>
#include <str_error.h>
#include <malloc.h>
#include <stdio.h>
#include <str.h>
#include <str_error.h>
#include <ns.h>
#include <ipc/services.h>
#include <ipc/net.h>
#include <ipc/net_net.h>
#include <ipc/il.h>
#include <ipc/ip.h>
#include <ipc/nil.h>
#include <net/packet.h>
#include <net/device.h>
#include <adt/char_map.h>
#include <adt/generic_char_map.h>
#include <adt/measured_strings.h>
#include <adt/module_map.h>
#include <loc.h>
#include <nic.h>
#include <nil_remote.h>
#include <net_interface.h>
#include <ip_interface.h>
#include <device/nic.h>
#include <dirent.h>
#include <fcntl.h>
#include <cfg.h>
#include "net.h"
#include "packet_server.h"

#define MAX_PATH_LENGTH  1024

/** Networking module global data. */
net_globals_t net_globals;

GENERIC_CHAR_MAP_IMPLEMENT(measured_strings, measured_string_t);
DEVICE_MAP_IMPLEMENT(netifs, netif_t);
LIST_INITIALIZE(netif_list);

/** Add the configured setting to the configuration map.
 *
 * @param[in] configuration The configuration map.
 * @param[in] name          The setting name.
 * @param[in] value         The setting value.
 *
 * @return EOK on success.
 * @return ENOMEM if there is not enough memory left.
 *
 */
static int add_configuration(measured_strings_t *configuration,
    const uint8_t *name, const uint8_t *value)
{
	int rc;
	
	measured_string_t *setting =
	    measured_string_create_bulk(value, 0);
	if (!setting)
		return ENOMEM;
	
	/* Add the configuration setting */
	rc = measured_strings_add(configuration, name, 0, setting);
	if (rc != EOK) {
		free(setting);
		return rc;
	}
	
	return EOK;
}

/** Generate new system-unique device identifier.
 *
 * @return The system-unique devic identifier.
 *
 */
static nic_device_id_t generate_new_device_id(void)
{
	return device_assign_devno();
}

static int read_configuration_file(const char *directory, const char *filename,
    measured_strings_t *configuration)
{
	printf("%s: Reading configuration file %s/%s\n", NAME, directory, filename);
	
	cfg_file_t cfg;
	int rc = cfg_load_path(directory, filename, &cfg);
	if (rc != EOK)
		return rc;
	
	if (cfg_anonymous(&cfg) == NULL) {
		cfg_unload(&cfg);
		return ENOENT;
	}
	
	cfg_section_foreach(cfg_anonymous(&cfg), link) {
		const cfg_entry_t *entry = cfg_entry_instance(link);
		
		rc = add_configuration(configuration,
		    (uint8_t *) entry->key, (uint8_t *) entry->value);
		if (rc != EOK) {
			cfg_unload(&cfg);
			return rc;
		}
	}
	
	cfg_unload(&cfg);
	return EOK;
}

/** Read the network interface specific configuration.
 *
 * @param[in]     name  The network interface name.
 * @param[in,out] netif The network interface structure.
 *
 * @return EOK on success.
 * @return Other error codes as defined for the add_configuration() function.
 *
 */
static int read_netif_configuration(const char *name, netif_t *netif)
{
	return read_configuration_file(CONF_DIR, name, &netif->configuration);
}

/** Read the networking subsystem global configuration.
 *
 * @return EOK on success.
 * @return Other error codes as defined for the add_configuration() function.
 *
 */
static int read_configuration(void)
{
	return read_configuration_file(CONF_DIR, CONF_GENERAL_FILE,
	    &net_globals.configuration);
}

/** Return the configured values.
 *
 * The network interface configuration is searched first.
 *
 * @param[in]  netif_conf    The network interface configuration setting.
 * @param[out] configuration The found configured values.
 * @param[in]  count         The desired settings count.
 * @param[out] data          The found configuration settings data.
 *
 * @return EOK.
 *
 */
static int net_get_conf(measured_strings_t *netif_conf,
    measured_string_t *configuration, size_t count)
{
	if ((!configuration) || (count <= 0))
			return EINVAL;
	
	size_t index;
	for (index = 0; index < count; index++) {
		measured_string_t *setting =
		    measured_strings_find(netif_conf, configuration[index].value, 0);
		if (!setting)
			setting = measured_strings_find(&net_globals.configuration,
			    configuration[index].value, 0);
		
		if (setting) {
			configuration[index].length = setting->length;
			configuration[index].value = setting->value;
		} else {
			configuration[index].length = 0;
			configuration[index].value = NULL;
		}
	}
	
	return EOK;
}

static int net_get_device_conf(nic_device_id_t device_id,
    measured_string_t *configuration, size_t count)
{
	netif_t *netif = netifs_find(&net_globals.netifs, device_id);
	if (netif)
		return net_get_conf(&netif->configuration, configuration, count);
	else
		return net_get_conf(NULL, configuration, count);
}

static int net_get_devices(measured_string_t **devices, size_t *dev_count)
{
	if (!devices)
		return EBADMEM;
	
	size_t max_count = netifs_count(&net_globals.netifs);
	*devices = malloc(max_count * sizeof(measured_string_t));
	if (*devices == NULL)
		return ENOMEM;
	
	size_t count = 0;
	for (size_t i = 0; i < max_count; i++) {
		netif_t *item = netifs_get_index(&net_globals.netifs, i);
		if (item->sess != NULL) {
			/* 
			 * Use format "device_id:device_name"
			 * FIXME: This typecasting looks really ugly
			 */
			(*devices)[count].length = asprintf(
			    (char **) &((*devices)[count].value),
			    NIC_DEVICE_PRINT_FMT ":%s", item->id,
			    (const char *) item->name);
			count++;
		}
	}
	
	*dev_count = (size_t) count;
	return EOK;
}

static int net_get_devices_count()
{
	size_t max_count = netifs_count(&net_globals.netifs);
	
	size_t count = 0;
	for (size_t i = 0; i < max_count; i++) {
		netif_t *item = netifs_get_index(&net_globals.netifs, i);
		if (item->sess != NULL)
			count++;
	}
	
	return count;
}

static void net_free_devices(measured_string_t *devices, size_t count)
{
	size_t i;
	for (i = 0; i < count; ++i)
		free(devices[i].value);
	
	free(devices);
}

/** Start the network interface according to its configuration.
 *
 * Register the network interface with the subsystem modules.
 * Start the needed subsystem modules.
 *
 * @param[in] netif The network interface specific data.
 *
 * @return EOK on success.
 * @return EINVAL if there are some settings missing.
 * @return ENOENT if the internet protocol module is not known.
 * @return Other error codes as defined for the netif_probe_req() function.
 * @return Other error codes as defined for the nil_device_req() function.
 * @return Other error codes as defined for the needed internet layer
 *         registering function.
 *
 */
static int init_device(netif_t *netif, service_id_t sid)
{
	printf("%s: Initializing device '%s'\n", NAME, netif->name);
	
	link_initialize(&netif->netif_list);
	
	netif->sid = sid;
	netif->sess = loc_service_connect(EXCHANGE_SERIALIZE, netif->sid,
	    IPC_FLAG_BLOCKING);
	if (netif->sess == NULL) {
		printf("%s: Unable to connect to device\n", NAME);
		return EREFUSED;
	}
	
	/* Optional network interface layer */
	measured_string_t *setting = measured_strings_find(&netif->configuration,
	    (uint8_t *) CONF_NIL, 0);
	if (setting) {
		netif->nil = get_running_module(&net_globals.modules,
		    setting->value);
		if (!netif->nil) {
			printf("%s: Unable to connect to network interface layer '%s'\n",
			    NAME, setting->value);
			return EINVAL;
		}
	} else
		netif->nil = NULL;
	
	/* Mandatory internet layer */
	setting = measured_strings_find(&netif->configuration,
	    (uint8_t *) CONF_IL, 0);
	netif->il = get_running_module(&net_globals.modules,
	    setting->value);
	if (!netif->il) {
		printf("%s: Unable to connect to internet layer '%s'\n",
		    NAME, setting->value);
		return EINVAL;
	}
	
	/* Network interface layer startup */
	int rc;
	services_t nil_service;
	if (netif->nil) {
		setting = measured_strings_find(&netif->configuration,
		    (uint8_t *) CONF_MTU, 0);
		if (!setting)
			setting = measured_strings_find(&net_globals.configuration,
			    (uint8_t *) CONF_MTU, 0);
		
		int mtu = setting ?
		    strtol((const char *) setting->value, NULL, 10) : 0;
		rc = nil_device_req(netif->nil->sess, netif->id,
		    netif->sid, mtu);
		if (rc != EOK) {
			printf("%s: Unable to start network interface layer\n",
			    NAME);
			return rc;
		}
		
		nil_service = netif->nil->service;
	} else
		nil_service = -1;
	
	/* Inter-network layer startup */
	switch (netif->il->service) {
	case SERVICE_IP:
		rc = ip_device_req(netif->il->sess, netif->id, nil_service);
		if (rc != EOK) {
			printf("%s: Unable to start internet layer\n", NAME);
			return rc;
		}
		
		break;
	default:
		return ENOENT;
	}
	
	printf("%s: Activating device '%s'\n", NAME, netif->name);
	list_append(&netif->netif_list, &netif_list);
	return nic_set_state(netif->sess, NIC_STATE_ACTIVE);
}

static int net_nic_ready(service_id_t sid)
{
	int rc;
	char *hwpath;
	
	rc = loc_service_get_name(sid, &hwpath);
	if (rc != EOK) {
		printf("%s: Failed getting name of service '%u'\n",
		    NAME, (unsigned) sid);
		return EINVAL;
	}
	
	int index = char_map_find(&net_globals.netif_hwpaths,
	    (uint8_t *) hwpath, 0);
	
	if (index == CHAR_MAP_NULL) {
		printf("%s: Service '%s' not found in map.\n", NAME, hwpath);
		free(hwpath);
		return ENOENT;
	}
	
	free(hwpath);
	
	netif_t *netif = netifs_get_index(&net_globals.netifs, index);
	if (netif == NULL)
		return ENOENT;
	
	rc = init_device(netif, sid);
	if (rc != EOK)
		return rc;
	
	/* Increment module usage */
	if (netif->nil)
		netif->nil->usage++;
	
	netif->il->usage++;
	
	return EOK;
}

/** Process the networking message.
 *
 * @param[in]  callid       The message identifier.
 * @param[in]  call         The message parameters.
 * @param[out] answer       The message answer parameters.
 * @param[out] answer_count The last parameter for the actual answer
 *                          in the answer parameter.
 *
 * @return EOK on success.
 * @return ENOTSUP if the message is not known.
 *
 * @see net_interface.h
 * @see IS_NET_NET_MESSAGE()
 *
 */
static int net_message(ipc_callid_t callid, ipc_call_t *call,
    ipc_call_t *answer, size_t *answer_count)
{
	measured_string_t *strings;
	uint8_t *data;
	int rc;
	size_t count;
	
	*answer_count = 0;
	
	if (!IPC_GET_IMETHOD(*call))
		return EOK;
	
	switch (IPC_GET_IMETHOD(*call)) {
	case NET_NET_GET_DEVICE_CONF:
		rc = measured_strings_receive(&strings, &data,
		    IPC_GET_COUNT(*call));
		if (rc != EOK)
			return rc;
		
		net_get_device_conf(IPC_GET_DEVICE(*call), strings,
		    IPC_GET_COUNT(*call));
		
		rc = measured_strings_reply(strings, IPC_GET_COUNT(*call));
		free(strings);
		free(data);
		return rc;
	case NET_NET_GET_CONF:
		rc = measured_strings_receive(&strings, &data,
		    IPC_GET_COUNT(*call));
		if (rc != EOK)
			return rc;
		
		net_get_conf(NULL, strings, IPC_GET_COUNT(*call));
		
		rc = measured_strings_reply(strings, IPC_GET_COUNT(*call));
		free(strings);
		free(data);
		return rc;
	case NET_NET_GET_DEVICES_COUNT:
		count = (size_t) net_get_devices_count();
		IPC_SET_ARG1(*answer, count);
		*answer_count = 1;
		return EOK;
	case NET_NET_GET_DEVICES:
		rc = net_get_devices(&strings, &count);
		if (rc != EOK)
			return rc;
		
		rc = measured_strings_reply(strings, count);
		net_free_devices(strings, count);
		return rc;
	default:
		return ENOTSUP;
	}
}

/** Default thread for new connections.
 *
 * @param[in] iid   The initial message identifier.
 * @param[in] icall The initial message call structure.
 * @param[in] arg   Local argument.
 *
 */
static void net_client_connection(ipc_callid_t iid, ipc_call_t *icall,
    void *arg)
{
	/*
	 * Accept the connection
	 *  - Answer the first IPC_M_CONNECT_ME_TO call.
	 */
	async_answer_0(iid, EOK);
	
	while (true) {
		/* Clear the answer structure */
		ipc_call_t answer;
		size_t count;
		refresh_answer(&answer, &count);
		
		/* Fetch the next message */
		ipc_call_t call;
		ipc_callid_t callid = async_get_call(&call);
		
		/* Process the message */
		int res;
		if (IS_NET_PACKET_MESSAGE(call))
			res = packet_server_message(callid, &call, &answer, &count);
		else
			res = net_message(callid, &call, &answer, &count);
		
		/* End if told to either by the message or the processing result */
		if ((!IPC_GET_IMETHOD(call)) || (res == EHANGUP))
			return;
		
		/* Answer the message */
		answer_call(callid, res, &answer, count);
	}
}

static int nic_check_new(void)
{
	category_id_t nic_cat;
	service_id_t *svcs;
	size_t count, i;
	bool already_known;
	int rc;

	rc = loc_category_get_id(DEVICE_CATEGORY_NIC, &nic_cat, IPC_FLAG_BLOCKING);
	if (rc != EOK) {
		printf("%s: Failed resolving category '%s'.\n", NAME,
		    DEVICE_CATEGORY_NIC);
		return ENOENT;
	}

	rc = loc_category_get_svcs(nic_cat, &svcs, &count);
	if (rc != EOK) {
		printf("%s: Failed getting list of NIC devices.\n", NAME);
		return EIO;
	}

	for (i = 0; i < count; i++) {
		already_known = false;

		list_foreach(netif_list, link) {
			netif_t *netif = list_get_instance(link, netif_t, netif_list);
			if (netif->sid == svcs[i]) {
				already_known = true;
				break;
			}
		}

		if (!already_known) {
			rc = net_nic_ready(svcs[i]);
			if (rc != EOK) {
				printf("%s: Failed adding NIC device #%u.\n",
				    NAME, (unsigned) svcs[i]);
			}
		}
	}

	free(svcs);
	return EOK;
}

static void cat_change_cb(void)
{
	(void) nic_check_new();
}

static int net_start_nic_discovery(void)
{
	int rc;

	rc = loc_register_cat_change_cb(cat_change_cb);
	if (rc != EOK) {
		printf("%s: Failed registering callback for device discovery (%d).\n",
		    NAME, rc);
		return rc;
	}

	return nic_check_new();
}

int main(int argc, char *argv[])
{
	netifs_initialize(&net_globals.netifs);
	char_map_initialize(&net_globals.netif_hwpaths);
	modules_initialize(&net_globals.modules);
	measured_strings_initialize(&net_globals.configuration);
	async_set_client_connection(net_client_connection);
	
	int rc = pm_init();
	if (rc != EOK) {
		printf("%s: Unable to initialize packet management\n", NAME);
		return rc;
	}
	
	rc = packet_server_init();
	if (rc != EOK) {
		printf("%s: Unable to initialize packet server\n", NAME);
		pm_destroy();
		return rc;
	}
	
	rc = read_configuration();
	if (rc != EOK) {
		printf("%s: Error reading configuration\n", NAME);
		pm_destroy();
		return rc;
	}
	
	DIR *config_dir = opendir(CONF_DIR);
	if (config_dir != NULL) {
		struct dirent *dir_entry;
		while ((dir_entry = readdir(config_dir))) {
			/* Ignore files without the CONF_EXT extension */
			if ((str_size(dir_entry->d_name) < str_size(CONF_EXT)) ||
			    (str_cmp(dir_entry->d_name + str_size(dir_entry->d_name) -
			    str_size(CONF_EXT), CONF_EXT) != 0))
				continue;
			
			
			netif_t *netif = (netif_t *) malloc(sizeof(netif_t));
			if (!netif)
				continue;
			
			netif->sid = -1;
			netif->sess = NULL;
			
			netif->id = generate_new_device_id();
			if (!netif->id) {
				free(netif);
				continue;
			}
			
			rc = measured_strings_initialize(&netif->configuration);
			if (rc != EOK) {
				free(netif);
				continue;
			}
			
			rc = read_netif_configuration(dir_entry->d_name, netif);
			if (rc != EOK) {
				printf("%s: Error reading configuration %s\n", NAME,
				    dir_entry->d_name);
				free(netif);
				continue;
			}
			
			measured_string_t *name = measured_strings_find(&netif->configuration,
			    (uint8_t *) CONF_NAME, 0);
			if (!name) {
				printf("%s: Network interface name is missing in %s\n",
				    NAME, dir_entry->d_name);
				measured_strings_destroy(&netif->configuration, free);
				free(netif);
				continue;
			}
			
			netif->name = name->value;
			
			/* Mandatory hardware path */
			measured_string_t *hwpath = measured_strings_find(
			    &netif->configuration, (const uint8_t *) CONF_HWPATH, 0);
			if (!hwpath) {
				printf("%s: Hardware path is missing in %s\n",
				    NAME, dir_entry->d_name);
				measured_strings_destroy(&netif->configuration, free);
				free(netif);
				continue;
			}
			
			int index = netifs_add(&net_globals.netifs, netif->id, netif);
			if (index < 0) {
				measured_strings_destroy(&netif->configuration, free);
				free(netif);
				continue;
			}
			
			/*
			 * Add to the hardware paths map and init network interfaces
			 * and needed modules.
			 */
			rc = char_map_add(&net_globals.netif_hwpaths, hwpath->value, 0, index);
			if (rc != EOK) {
				measured_strings_destroy(&netif->configuration, free);
				netifs_exclude_index(&net_globals.netifs, index, free);
				continue;
			}
		}
		
		closedir(config_dir);
	}
	
	rc = add_module(NULL, &net_globals.modules, (uint8_t *) ETHERNET_NAME,
	    (uint8_t *) ETHERNET_FILENAME, SERVICE_ETHERNET, 0, connect_to_service);
	if (rc != EOK) {
		printf("%s: Error adding module '%s'\n", NAME, ETHERNET_NAME);
		pm_destroy();
		return rc;
	}
	
	rc = add_module(NULL, &net_globals.modules, (uint8_t *) NILDUMMY_NAME,
	    (uint8_t *) NILDUMMY_FILENAME, SERVICE_NILDUMMY, 0, connect_to_service);
	if (rc != EOK) {
		printf("%s: Error adding module '%s'\n", NAME, NILDUMMY_NAME);
		pm_destroy();
		return rc;
	}
	
	task_id_t task_id = net_spawn((uint8_t *) IP_FILENAME);
	if (!task_id) {
		printf("%s: Error spawning IP module\n", NAME);
		pm_destroy();
		return EINVAL;
	}
	
	rc = add_module(NULL, &net_globals.modules, (uint8_t *) IP_NAME,
	    (uint8_t *) IP_FILENAME, SERVICE_IP, task_id, ip_connect_module);
	if (rc != EOK) {
		printf("%s: Error adding module '%s'\n", NAME, IP_NAME);
		pm_destroy();
		return rc;
	}
	
	if (!net_spawn((uint8_t *) "/srv/icmp")) {
		printf("%s: Error spawning ICMP module\n", NAME);
		pm_destroy();
		return EINVAL;
	}
	
	if (!net_spawn((uint8_t *) "/srv/udp")) {
		printf("%s: Error spawning UDP module\n", NAME);
		pm_destroy();
		return EINVAL;
	}
	
	if (!net_spawn((uint8_t *) "/srv/tcp")) {
		printf("%s: Error spawning TCP module\n", NAME);
		pm_destroy();
		return EINVAL;
	}
	
	rc = service_register(SERVICE_NETWORKING);
	if (rc != EOK) {
		printf("%s: Error registering service\n", NAME);
		pm_destroy();
		return rc;
	}
	
	rc = net_start_nic_discovery();
	if (rc != EOK) {
		printf("%s: Error starting NIC discovery\n", NAME);
		pm_destroy();
		return rc;
	}
	
	task_retval(0);
	async_manager();
	return 0;
}

/** @}
 */
