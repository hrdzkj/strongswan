/*
 * Copyright (C) 2011 Andreas Steffen 
 * HSR Hochschule fuer Technik Rapperswil
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

#include "tnc_ifmap_soap.h"

#include <debug.h>

#include <axis2_util.h>
#include <axis2_client.h>
#include <axiom_soap.h>

#define IFMAP_NS	  "http://www.trustedcomputinggroup.org/2010/IFMAP/2"
#define IFMAP_META_NS "http://www.trustedcomputinggroup.org/2010/IFMAP-METADATA/2"
#define IFMAP_LOGFILE "strongswan_ifmap.log"
#define IFMAP_SERVER  "https://localhost:8443/"
	
typedef struct private_tnc_ifmap_soap_t private_tnc_ifmap_soap_t;

/**
 * Private data of an tnc_ifmap_soap_t object.
 */
struct private_tnc_ifmap_soap_t {

	/**
	 * Public tnc_ifmap_soap_t interface.
	 */
	tnc_ifmap_soap_t public;

	/**
	 * Axis2/C environment 
	 */
	axutil_env_t *env;

	/**
	 * Axis2 service client
	 */
	axis2_svc_client_t* svc_client;

	/**
	 * SOAP Session ID
	 */
	char *session_id;

	/**
	 * IF-MAP Publisher ID
	 */
	char *ifmap_publisher_id;

	/**
	 * PEP and PDP device name
	 */
	char *device_name;

};

/**
 * Send request and receive result via SOAP
 */
static bool send_receive(private_tnc_ifmap_soap_t *this,
						 char *request_qname, axiom_node_t *request,
						 char *receipt_qname, axiom_node_t **result)

{
    axiom_node_t *parent, *node;
    axiom_element_t *el;
	axutil_qname_t *qname;
	bool success = FALSE;

	/* send request and receive result */
	DBG2(DBG_TNC, "sending  ifmap %s", request_qname);

	parent = axis2_svc_client_send_receive(this->svc_client, this->env, request);
	if (!parent)
	{
		DBG1(DBG_TNC, "no ifmap %s received from MAP server", receipt_qname);
		return FALSE;
	}

	/* pre-process result */
	node = axiom_node_get_first_child(parent, this->env);
	if (node && axiom_node_get_node_type(node, this->env) == AXIOM_ELEMENT)
	{
		el = (axiom_element_t *)axiom_node_get_data_element(node, this->env);

		qname = axiom_element_get_qname(el, this->env, node);
		success = streq(receipt_qname, axutil_qname_to_string(qname, this->env));
		if (success)
 		{
			DBG2(DBG_TNC, "received ifmap %s", receipt_qname);
			if (result)
			{
				*result = parent;
			}
			else
			{
				/* no further processing requested */
				axiom_node_free_tree(parent, this->env);
			}
			return TRUE;
		}
		/* TODO proper error handling */
		DBG1(DBG_TNC, "%s", axiom_element_to_string(el, this->env, node));
	}

	/* free parent in the error case */
	axiom_node_free_tree(parent, this->env);

	return FALSE;
}

METHOD(tnc_ifmap_soap_t, newSession, bool,
	private_tnc_ifmap_soap_t *this)
{
    axiom_node_t *request, *result, *node;
    axiom_element_t *el;
	axiom_namespace_t *ns;
	axis2_char_t *value;


	/* build newSession request */
    ns = axiom_namespace_create(this->env, IFMAP_NS, "ifmap");
    el = axiom_element_create(this->env, NULL, "newSession", ns, &request);

	/* send newSession request and receive newSessionResult */
	if (!send_receive(this, "newSession", request, "newSessionResult", &result))
	{
		return FALSE;
	}

	/* process newSessionResult */
	node = axiom_node_get_first_child(result, this->env);
	el = (axiom_element_t *)axiom_node_get_data_element(node, this->env);

	/* get session-id */
	value = axiom_element_get_attribute_value_by_name(el, this->env,
								 "session-id");
	this->session_id = strdup(value);

	/* get ifmap-publisher-id */
	value = axiom_element_get_attribute_value_by_name(el, this->env,
								 "ifmap-publisher-id");
	this->ifmap_publisher_id = strdup(value);

	DBG1(DBG_TNC, "session-id: %s, ifmap-publisher-id: %s",
				   this->session_id, this->ifmap_publisher_id);

	/* set PEP and PDP device name (defaults to IF-MAP Publisher ID) */
	this->device_name = lib->settings->get_str(lib->settings,
								"charon.plugins.tnc-ifmap.device_name",
								 this->ifmap_publisher_id);
	this->device_name = strdup(this->device_name);

	/* free result */
	axiom_node_free_tree(result, this->env);

    return this->session_id && this->ifmap_publisher_id;
}

METHOD(tnc_ifmap_soap_t, purgePublisher, bool,
	private_tnc_ifmap_soap_t *this)
{
	axiom_node_t *request;
	axiom_element_t *el;
	axiom_namespace_t *ns;
	axiom_attribute_t *attr;

	/* build purgePublisher request */
 	ns = axiom_namespace_create(this->env, IFMAP_NS, "ifmap");
	el = axiom_element_create(this->env, NULL, "purgePublisher", ns, &request);
	attr = axiom_attribute_create(this->env, "session-id",
								  this->session_id, NULL);	
	axiom_element_add_attribute(el, this->env, attr, request);
	attr = axiom_attribute_create(this->env, "ifmap-publisher-id",
								  this->ifmap_publisher_id, NULL);	
	axiom_element_add_attribute(el, this->env, attr, request);

	/* send purgePublisher request and receive purgePublisherReceived */
	return send_receive(this, "purgePublisher", request,
							  "purgePublisherReceived", NULL);
}

/**
 * Create an access-request based on device_name and ike_sa_id
 */
static axiom_node_t* create_access_request(private_tnc_ifmap_soap_t *this,
										   u_int32_t id)
{
	axiom_element_t *el;
	axiom_node_t *node;
	axiom_attribute_t *attr;
	char buf[BUF_LEN];

	el = axiom_element_create(this->env, NULL, "access-request", NULL, &node);

	snprintf(buf, BUF_LEN, "%s:%d", this->device_name, id);
	attr = axiom_attribute_create(this->env, "name", buf, NULL);	
	axiom_element_add_attribute(el, this->env, attr, node);

	return node;
}

/**
 * Create an identity
 */
static axiom_node_t* create_identity(private_tnc_ifmap_soap_t *this,
									 identification_t *id, bool is_user)
{
	axiom_element_t *el;
	axiom_node_t *node;
	axiom_attribute_t *attr;
	char buf[BUF_LEN], *id_type;

	el = axiom_element_create(this->env, NULL, "identity", NULL, &node);

	snprintf(buf, BUF_LEN, "%Y", id);
	attr = axiom_attribute_create(this->env, "name", buf, NULL);	
	axiom_element_add_attribute(el, this->env, attr, node);

	switch (id->get_type(id))
	{
		case ID_FQDN:
			id_type = is_user ? "username" : "dns-name";
			break;
		case ID_RFC822_ADDR:
			id_type = "email-address";
			break;
		case ID_DER_ASN1_DN:
			id_type = "distinguished-name";
			break;
		default:
			id_type = "other";
	}
	attr = axiom_attribute_create(this->env, "type", id_type, NULL);	
	axiom_element_add_attribute(el, this->env, attr, node);

	return node;
}

/**
 * Create an ip-address
 */
static axiom_node_t* create_ip_address(private_tnc_ifmap_soap_t *this,
									   host_t *host)
{
	axiom_element_t *el;
	axiom_node_t *node;
	axiom_attribute_t *attr;
	char buf[BUF_LEN];

	el = axiom_element_create(this->env, NULL, "ip-address", NULL, &node);

	if (host->get_family(host) == AF_INET6)
	{
		chunk_t address;
		int len, written, i;
		char *pos;
		bool first = TRUE;

		/* output IPv6 address in canonical IF-MAP 2.0 format */
		address = host->get_address(host);
		pos = buf;
		len = sizeof(buf);

		for (i = 0; i < address.len; i = i + 2)
		{
			written = snprintf(pos, len, "%s%x", first ? "" : ":",
							   256*address.ptr[i] +  address.ptr[i+1]);
			if (written < 0 || written > len)
			{
				break;
			}
			pos += written;
			len -= written;
			first = FALSE;
		}
	}
	else
	{
		snprintf(buf, BUF_LEN, "%H", host);
	}
	attr = axiom_attribute_create(this->env, "value", buf, NULL);	
	axiom_element_add_attribute(el, this->env, attr, node);

	attr = axiom_attribute_create(this->env, "type",
				 host->get_family(host) == AF_INET ? "IPv4" : "IPv6", NULL);	
	axiom_element_add_attribute(el, this->env, attr, node);

	return node;
}

/**
 * Create a device
 */
static axiom_node_t* create_device(private_tnc_ifmap_soap_t *this)
{
	axiom_element_t *el;
	axiom_node_t *node, *node2, *node3;
	axiom_text_t *text;

	el = axiom_element_create(this->env, NULL, "device", NULL, &node);
	el = axiom_element_create(this->env, NULL, "name", NULL, &node2);
	axiom_node_add_child(node, this->env, node2);
	text = axiom_text_create(this->env, node2, this->device_name, &node3);

	return node;
}

/**
 * Create metadata
 */
static axiom_node_t* create_metadata(private_tnc_ifmap_soap_t *this,
									 char *metadata)
{
	axiom_element_t *el;
	axiom_node_t *node, *node2;
	axiom_attribute_t *attr;
	axiom_namespace_t *ns_meta;

	el = axiom_element_create(this->env, NULL, "metadata", NULL, &node);
	ns_meta = axiom_namespace_create(this->env, IFMAP_META_NS, "meta");

	el = axiom_element_create(this->env, NULL, metadata, ns_meta, &node2);
	axiom_node_add_child(node, this->env, node2);
	attr = axiom_attribute_create(this->env, "ifmap-cardinality", "singleValue",
								  NULL);	
	axiom_element_add_attribute(el, this->env, attr, node2);

	return node;
}

/**
 * Create delete filter
 */
static axiom_node_t* create_delete_filter(private_tnc_ifmap_soap_t *this,
										  char *metadata)
{
	axiom_element_t *el;
	axiom_node_t *node;
	axiom_attribute_t *attr;
	char buf[BUF_LEN];

	el = axiom_element_create(this->env, NULL, "delete", NULL, &node);

	snprintf(buf, BUF_LEN, "meta:%s[@ifmap-publisher-id='%s']",
			 metadata, this->ifmap_publisher_id);
	attr = axiom_attribute_create(this->env, "filter", buf, NULL);	
	axiom_element_add_attribute(el, this->env, attr, node);

	return node;
}

METHOD(tnc_ifmap_soap_t, publish_ike_sa, bool,
	private_tnc_ifmap_soap_t *this, u_int32_t ike_sa_id, identification_t *id,
	bool is_user, host_t *host, bool up)
{
	axiom_node_t *request, *node;
	axiom_element_t *el;
	axiom_namespace_t *ns, *ns_meta;
	axiom_attribute_t *attr;

	/* build publish request */
 	ns = axiom_namespace_create(this->env, IFMAP_NS, "ifmap");
 	el = axiom_element_create(this->env, NULL, "publish", ns, &request);
	ns_meta = axiom_namespace_create(this->env, IFMAP_META_NS, "meta");
	axiom_element_declare_namespace(el, this->env, request, ns_meta);	
	attr = axiom_attribute_create(this->env, "session-id", this->session_id,
								  NULL);	
	axiom_element_add_attribute(el, this->env, attr, request);

	/**
	 * update or delete authenticated-as metadata
	 */
 	if (up)
	{
		el = axiom_element_create(this->env, NULL, "update", NULL, &node);
	}
	else
	{
		node = create_delete_filter(this, "authenticated-as");
	}
	axiom_node_add_child(request, this->env, node);

	/* add access-request, identity and [if up] metadata */
	axiom_node_add_child(node, this->env,
						 	 create_access_request(this, ike_sa_id));
	axiom_node_add_child(node, this->env,
							 create_identity(this, id, is_user));
	if (up)
	{
		axiom_node_add_child(node, this->env,
							 create_metadata(this, "authenticated-as"));
	}

	/**
	 * update or delete access-request-ip metadata
	 */
 	if (up)
	{
		el = axiom_element_create(this->env, NULL, "update", NULL, &node);
	}
	else
	{
		node = create_delete_filter(this, "access-request-ip");
	}
	axiom_node_add_child(request, this->env, node);

	/* add access-request, ip-address and [if up] metadata */
	axiom_node_add_child(node, this->env,
							 create_access_request(this, ike_sa_id));
	axiom_node_add_child(node, this->env,
							 create_ip_address(this, host));
	if (up)
	{
		axiom_node_add_child(node, this->env,
							 create_metadata(this, "access-request-ip"));
	}

	/**
	 * update or delete authenticated-by metadata
	 */
 	if (up)
	{
		el = axiom_element_create(this->env, NULL, "update", NULL, &node);
	}
	else
	{
		node = create_delete_filter(this, "authenticated-by");
	}
	axiom_node_add_child(request, this->env, node);

	/* add access-request, device and [if up] metadata */
	axiom_node_add_child(node, this->env,
							 create_access_request(this, ike_sa_id));
	axiom_node_add_child(node, this->env,
							 create_device(this));
	if (up)
	{
		axiom_node_add_child(node, this->env,
							 create_metadata(this, "authenticated-by"));
	}

	/* send publish request and receive publishReceived */
	return send_receive(this, "publish", request, "publishReceived", NULL);
}

METHOD(tnc_ifmap_soap_t, publish_device_ip, bool,
	private_tnc_ifmap_soap_t *this, host_t *host)
{
	axiom_node_t *request, *node;
	axiom_element_t *el;
	axiom_namespace_t *ns, *ns_meta;
	axiom_attribute_t *attr;

	/* build publish request */
 	ns = axiom_namespace_create(this->env, IFMAP_NS, "ifmap");
 	el = axiom_element_create(this->env, NULL, "publish", ns, &request);
	ns_meta = axiom_namespace_create(this->env, IFMAP_META_NS, "meta");
	axiom_element_declare_namespace(el, this->env, request, ns_meta);	
	attr = axiom_attribute_create(this->env, "session-id", this->session_id,
								  NULL);	
	axiom_element_add_attribute(el, this->env, attr, request);
	el = axiom_element_create(this->env, NULL, "update", NULL, &node);
	axiom_node_add_child(request, this->env, node);

	/* add device, ip-address and metadata */
	axiom_node_add_child(node, this->env,
							 create_device(this));
	axiom_node_add_child(node, this->env,
							 create_ip_address(this, host));
	axiom_node_add_child(node, this->env,
							 create_metadata(this, "device-ip"));

	/* send publish request and receive publishReceived */
	return send_receive(this, "publish", request, "publishReceived", NULL);
}

METHOD(tnc_ifmap_soap_t, endSession, bool,
	private_tnc_ifmap_soap_t *this)
{
	axiom_node_t *request;
	axiom_element_t *el;
	axiom_namespace_t *ns;
	axiom_attribute_t *attr;

	/* build endSession request */
 	ns = axiom_namespace_create(this->env, IFMAP_NS, "ifmap");
	el = axiom_element_create(this->env, NULL, "endSession", ns, &request);
	attr = axiom_attribute_create(this->env, "session-id", this->session_id, NULL);	
	axiom_element_add_attribute(el, this->env, attr, request);

	/* send endSession request and receive end SessionResult */
	return send_receive(this, "endSession", request, "endSessionResult", NULL);
}

METHOD(tnc_ifmap_soap_t, destroy, void,
	private_tnc_ifmap_soap_t *this)
{
	if (this->session_id)
	{
		endSession(this);
		free(this->session_id);
		free(this->ifmap_publisher_id);
		free(this->device_name);	
	}
	if (this->svc_client)
	{
		axis2_svc_client_free(this->svc_client, this->env);
	}
	if (this->env)
	{
		axutil_env_free(this->env);
	}
	free(this);
}

/**
 * See header
 */
tnc_ifmap_soap_t *tnc_ifmap_soap_create()
{
	private_tnc_ifmap_soap_t *this;
	axis2_char_t *server, *client_home, *username, *password, *auth_type;
	axis2_endpoint_ref_t* endpoint_ref = NULL;
	axis2_options_t *options = NULL;

	client_home = lib->settings->get_str(lib->settings,
					"charon.plugins.tnc-ifmap.client_home",
					AXIS2_GETENV("AXIS2C_HOME"));
	server = lib->settings->get_str(lib->settings,
					"charon.plugins.tnc-ifmap.server", IFMAP_SERVER);
	auth_type = lib->settings->get_str(lib->settings,
					"charon.plugins.tnc-ifmap.auth_type", "Basic");
	username = lib->settings->get_str(lib->settings,
					"charon.plugins.tnc-ifmap.username", NULL);
	password = lib->settings->get_str(lib->settings,
					"charon.plugins.tnc-ifmap.password", NULL);

	if (!username || !password)
	{
		DBG1(DBG_TNC, "MAP client %s%s%s not defined",
			(!username) ? "username" : "",
			(!username && ! password) ? " and " : "",
			(!password) ? "password" : "");
		return NULL;
	}

	INIT(this,
		.public = {
			.newSession = _newSession,
			.purgePublisher = _purgePublisher,
			.publish_ike_sa = _publish_ike_sa,
			.publish_device_ip = _publish_device_ip,
			.endSession = _endSession,
			.destroy = _destroy,
		},
	);

	/* Create Axis2/C environment and options */
	this->env = axutil_env_create_all(IFMAP_LOGFILE, AXIS2_LOG_LEVEL_TRACE);
    options = axis2_options_create(this->env);
 
	/* Define the IF-MAP server as the to endpoint reference */
	endpoint_ref = axis2_endpoint_ref_create(this->env, server);
	axis2_options_set_to(options, this->env, endpoint_ref);

	/* Create the axis2 service client */
	this->svc_client = axis2_svc_client_create(this->env, client_home);
	if (!this->svc_client)
	{
		DBG1(DBG_TNC, "could not create axis2 service client");
		AXIS2_LOG_ERROR(this->env->log, AXIS2_LOG_SI,
					    "Stub invoke FAILED: Error code: %d :: %s",
						this->env->error->error_number,
						AXIS2_ERROR_GET_MESSAGE(this->env->error));
		destroy(this);
		return NULL;
	}

	axis2_svc_client_set_options(this->svc_client, this->env, options);
	axis2_options_set_http_auth_info(options, this->env, username, password,
									 auth_type);
	DBG1(DBG_TNC, "connecting as MAP client '%s' to MAP server at '%s'",
		 username, server);

	return &this->public;
}

