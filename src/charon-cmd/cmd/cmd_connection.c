/*
 * Copyright (C) 2013 Martin Willi
 * Copyright (C) 2013 revosec AG
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.  See <http://www.fsf.org/copyleft/gpl.txt>.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * for more details.
 */

#include "cmd_connection.h"

#include <signal.h>
#include <unistd.h>

#include <utils/debug.h>
#include <processing/jobs/callback_job.h>
#include <threading/thread.h>
#include <daemon.h>

typedef enum profile_t profile_t;
typedef struct private_cmd_connection_t private_cmd_connection_t;

/**
 * Connection profiles we support
 */
enum profile_t {
	PROF_UNDEF,
	PROF_V2_PUB,
	PROF_V2_EAP,
	PROF_V2_PUB_EAP,
	PROF_V1_PUB,
	PROF_V1_XAUTH,
	PROF_V1_XAUTH_PSK,
	PROF_V1_HYBRID,
};

ENUM(profile_names, PROF_V2_PUB, PROF_V1_HYBRID,
	"ikev2-pub",
	"ikev2-eap",
	"ikev2-pub-eap",
	"ikev1-pub",
	"ikev1-xauth",
	"ikev1-xauth-psk",
	"ikev1-hybrid",
);

/**
 * Private data of an cmd_connection_t object.
 */
struct private_cmd_connection_t {

	/**
	 * Public cmd_connection_t interface.
	 */
	cmd_connection_t public;

	/**
	 * Process ID to terminate on failure
	 */
	pid_t pid;

	/**
	 * List of local traffic selectors
	 */
	linked_list_t *local_ts;

	/**
	 * List of remote traffic selectors
	 */
	linked_list_t *remote_ts;

	/**
	 * Hostname to connect to
	 */
	char *host;

	/**
	 * Server identity, or NULL to use host
	 */
	char *server;

	/**
	 * Local identity
	 */
	char *identity;

	/**
	 * Is a private key configured
	 */
	bool key_seen;

	/**
	 * Selected connection profile
	 */
	profile_t profile;
};

/**
 * Shut down application
 */
static void terminate(private_cmd_connection_t *this)
{
	kill(this->pid, SIGUSR1);
}

/**
 * Create peer config with associated ike config
 */
static peer_cfg_t* create_peer_cfg(private_cmd_connection_t *this)
{
	ike_cfg_t *ike_cfg;
	peer_cfg_t *peer_cfg;
	u_int16_t local_port, remote_port = IKEV2_UDP_PORT;
	ike_version_t version = IKE_ANY;

	switch (this->profile)
	{
		case PROF_UNDEF:
		case PROF_V2_PUB:
		case PROF_V2_EAP:
		case PROF_V2_PUB_EAP:
			version = IKEV2;
			break;
		case PROF_V1_PUB:
		case PROF_V1_XAUTH:
		case PROF_V1_XAUTH_PSK:
		case PROF_V1_HYBRID:
			version = IKEV1;
			break;
	}

	local_port = charon->socket->get_port(charon->socket, FALSE);
	if (local_port != IKEV2_UDP_PORT)
	{
		remote_port = IKEV2_NATT_PORT;
	}
	ike_cfg = ike_cfg_create(version, TRUE, FALSE, "0.0.0.0", FALSE, local_port,
					this->host, FALSE, remote_port, FRAGMENTATION_NO, 0);
	ike_cfg->add_proposal(ike_cfg, proposal_create_default(PROTO_IKE));
	peer_cfg = peer_cfg_create("cmd", ike_cfg,
					CERT_SEND_IF_ASKED, UNIQUE_REPLACE, 1, /* keyingtries */
					36000, 0, /* rekey 10h, reauth none */
					600, 600, /* jitter, over 10min */
					TRUE, FALSE, /* mobike, aggressive */
					30, 0, /* DPD delay, timeout */
					FALSE, NULL, NULL); /* mediation */
	peer_cfg->add_virtual_ip(peer_cfg, host_create_from_string("0.0.0.0", 0));

	return peer_cfg;
}

/**
 * Add a single auth cfg of given class to peer cfg
 */
static void add_auth_cfg(private_cmd_connection_t *this, peer_cfg_t *peer_cfg,
						 bool local, auth_class_t class)
{
	identification_t *id;
	auth_cfg_t *auth;

	auth = auth_cfg_create();
	auth->add(auth, AUTH_RULE_AUTH_CLASS, class);
	if (local)
	{
		id = identification_create_from_string(this->identity);
	}
	else
	{
		if (this->server)
		{
			id = identification_create_from_string(this->server);
		}
		else
		{
			id = identification_create_from_string(this->host);
		}
	}
	auth->add(auth, AUTH_RULE_IDENTITY, id);
	peer_cfg->add_auth_cfg(peer_cfg, auth, local);
}

/**
 * Attach authentication configs to peer config
 */
static bool add_auth_cfgs(private_cmd_connection_t *this, peer_cfg_t *peer_cfg)
{
	if (this->profile == PROF_UNDEF)
	{
		if (this->key_seen)
		{
			this->profile = PROF_V2_PUB;
		}
		else
		{
			this->profile = PROF_V2_EAP;
		}
	}
	switch (this->profile)
	{
		case PROF_V2_PUB:
		case PROF_V2_PUB_EAP:
		case PROF_V1_PUB:
		case PROF_V1_XAUTH:
			if (!this->key_seen)
			{
				DBG1(DBG_CFG, "missing private key for profile %N",
					 profile_names, this->profile);
				return FALSE;
			}
			break;
		default:
			break;
	}

	switch (this->profile)
	{
		case PROF_V2_PUB:
			add_auth_cfg(this, peer_cfg, TRUE, AUTH_CLASS_PUBKEY);
			add_auth_cfg(this, peer_cfg, FALSE, AUTH_CLASS_ANY);
			break;
		case PROF_V2_EAP:
			add_auth_cfg(this, peer_cfg, TRUE, AUTH_CLASS_EAP);
			add_auth_cfg(this, peer_cfg, FALSE, AUTH_CLASS_ANY);
			break;
		case PROF_V2_PUB_EAP:
			add_auth_cfg(this, peer_cfg, TRUE, AUTH_CLASS_PUBKEY);
			add_auth_cfg(this, peer_cfg, TRUE, AUTH_CLASS_EAP);
			add_auth_cfg(this, peer_cfg, FALSE, AUTH_CLASS_ANY);
			break;
		case PROF_V1_PUB:
			add_auth_cfg(this, peer_cfg, TRUE, AUTH_CLASS_PUBKEY);
			add_auth_cfg(this, peer_cfg, FALSE, AUTH_CLASS_PUBKEY);
			break;
		case PROF_V1_XAUTH:
			add_auth_cfg(this, peer_cfg, TRUE, AUTH_CLASS_PUBKEY);
			add_auth_cfg(this, peer_cfg, TRUE, AUTH_CLASS_XAUTH);
			add_auth_cfg(this, peer_cfg, FALSE, AUTH_CLASS_PUBKEY);
			break;
		case PROF_V1_XAUTH_PSK:
			add_auth_cfg(this, peer_cfg, TRUE, AUTH_CLASS_PSK);
			add_auth_cfg(this, peer_cfg, TRUE, AUTH_CLASS_XAUTH);
			add_auth_cfg(this, peer_cfg, FALSE, AUTH_CLASS_PSK);
			break;
		case PROF_V1_HYBRID:
			add_auth_cfg(this, peer_cfg, TRUE, AUTH_CLASS_XAUTH);
			add_auth_cfg(this, peer_cfg, FALSE, AUTH_CLASS_PUBKEY);
			break;
		default:
			return FALSE;
	}
	return TRUE;
}

/**
 * Attach child config to peer config
 */
static child_cfg_t* create_child_cfg(private_cmd_connection_t *this)
{
	child_cfg_t *child_cfg;
	traffic_selector_t *ts;
	lifetime_cfg_t lifetime = {
		.time = {
			.life = 10800 /* 3h */,
			.rekey = 10200 /* 2h50min */,
			.jitter = 300 /* 5min */
		}
	};

	child_cfg = child_cfg_create("cmd", &lifetime,
								 NULL, FALSE, MODE_TUNNEL, /* updown, hostaccess */
								 ACTION_NONE, ACTION_NONE, ACTION_NONE, FALSE,
								 0, 0, NULL, NULL, 0);
	child_cfg->add_proposal(child_cfg, proposal_create_default(PROTO_ESP));
	while (this->local_ts->remove_first(this->local_ts, (void**)&ts) == SUCCESS)
	{
		child_cfg->add_traffic_selector(child_cfg, TRUE, ts);
	}
	if (this->remote_ts->get_count(this->remote_ts) == 0)
	{
		/* add a 0.0.0.0/0 TS for remote side if none given */
		ts = traffic_selector_create_from_string(0, TS_IPV4_ADDR_RANGE,
									"0.0.0.0", 0, "255.255.255.255", 65535);
		this->remote_ts->insert_last(this->remote_ts, ts);
	}
	while (this->remote_ts->remove_first(this->remote_ts,
										 (void**)&ts) == SUCCESS)
	{
		child_cfg->add_traffic_selector(child_cfg, FALSE, ts);
	}

	return child_cfg;
}

/**
 * Initiate the configured connection
 */
static job_requeue_t initiate(private_cmd_connection_t *this)
{
	peer_cfg_t *peer_cfg;
	child_cfg_t *child_cfg;

	if (!this->host)
	{
		DBG1(DBG_CFG, "unable to initiate, missing --host option");
		terminate(this);
		return JOB_REQUEUE_NONE;
	}
	if (!this->identity)
	{
		DBG1(DBG_CFG, "unable to initiate, missing --identity option");
		terminate(this);
		return JOB_REQUEUE_NONE;
	}

	peer_cfg = create_peer_cfg(this);

	if (!add_auth_cfgs(this, peer_cfg))
	{
		peer_cfg->destroy(peer_cfg);
		terminate(this);
		return JOB_REQUEUE_NONE;
	}

	child_cfg = create_child_cfg(this);
	peer_cfg->add_child_cfg(peer_cfg, child_cfg->get_ref(child_cfg));

	if (charon->controller->initiate(charon->controller, peer_cfg, child_cfg,
									 controller_cb_empty, NULL, 0) != SUCCESS)
	{
		terminate(this);
	}
	return JOB_REQUEUE_NONE;
}

/**
 * Create a traffic selector from string, add to list
 */
static void add_ts(private_cmd_connection_t *this,
				   linked_list_t *list, char *string)
{
	traffic_selector_t *ts;

	ts = traffic_selector_create_from_cidr(string, 0, 0, 65535);
	if (!ts)
	{
		DBG1(DBG_CFG, "invalid traffic selector: %s", string);
		exit(1);
	}
	list->insert_last(list, ts);
}

/**
 * Parse profile name identifier
 */
static void set_profile(private_cmd_connection_t *this, char *name)
{
	int profile;

	profile = enum_from_name(profile_names, name);
	if (profile == -1)
	{
		DBG1(DBG_CFG, "unknown connection profile: %s", name);
		exit(1);
	}
	this->profile = profile;
}

METHOD(cmd_connection_t, handle, bool,
	private_cmd_connection_t *this, cmd_option_type_t opt, char *arg)
{
	switch (opt)
	{
		case CMD_OPT_HOST:
			this->host = arg;
			break;
		case CMD_OPT_REMOTE_IDENTITY:
			this->server = arg;
			break;
		case CMD_OPT_IDENTITY:
			this->identity = arg;
			break;
		case CMD_OPT_RSA:
			this->key_seen = TRUE;
			break;
		case CMD_OPT_LOCAL_TS:
			add_ts(this, this->local_ts, arg);
			break;
		case CMD_OPT_REMOTE_TS:
			add_ts(this, this->remote_ts, arg);
			break;
		case CMD_OPT_PROFILE:
			set_profile(this, arg);
			break;
		default:
			return FALSE;
	}
	return TRUE;
}

METHOD(cmd_connection_t, destroy, void,
	private_cmd_connection_t *this)
{
	this->local_ts->destroy_offset(this->local_ts,
								offsetof(traffic_selector_t, destroy));
	this->remote_ts->destroy_offset(this->remote_ts,
								offsetof(traffic_selector_t, destroy));
	free(this);
}

/**
 * See header
 */
cmd_connection_t *cmd_connection_create()
{
	private_cmd_connection_t *this;

	INIT(this,
		.public = {
			.handle = _handle,
			.destroy = _destroy,
		},
		.pid = getpid(),
		.local_ts = linked_list_create(),
		.remote_ts = linked_list_create(),
		.profile = PROF_UNDEF,
	);

	/* always include the virtual IP in traffic selector list */
	this->local_ts->insert_last(this->local_ts,
								traffic_selector_create_dynamic(0, 0, 65535));

	/* queue job, gets initiated as soon as we are up and running */
	lib->processor->queue_job(lib->processor,
		(job_t*)callback_job_create_with_prio(
			(callback_job_cb_t)initiate, this, NULL,
			(callback_job_cancel_t)return_false, JOB_PRIO_CRITICAL));

	return &this->public;
}