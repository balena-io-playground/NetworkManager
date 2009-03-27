/* -*- Mode: C; tab-width: 4; indent-tabs-mode: t; c-basic-offset: 4 -*- */
/* NetworkManager system settings service - keyfile plugin
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Copyright (C) 2009 Red Hat, Inc.
 */

#include <ctype.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>

#include <nm-setting-connection.h>
#include <nm-setting-wired.h>
#include <nm-setting-wireless.h>
#include <nm-setting-8021x.h>
#include <nm-setting-ip4-config.h>

#include "common.h"
#include "shvar.h"
#include "reader.h"
#include "writer.h"
#include "utils.h"

#define PLUGIN_WARN(pname, fmt, args...) \
	{ g_warning ("   " pname ": " fmt, ##args); }

static void
set_secret (shvarFile *ifcfg, const char *key, const char *value)
{
	shvarFile *keyfile;
	
	keyfile = utils_get_keys_ifcfg (ifcfg->fileName, TRUE);
	if (!keyfile) {
		PLUGIN_WARN (IFCFG_PLUGIN_NAME, "    warning: could not create key file for '%s'",
		             ifcfg->fileName);
		goto error;
	}

	/* Clear the secret from the actual ifcfg */
	svSetValue (ifcfg, key, NULL, FALSE);

	svSetValue (keyfile, key, value, FALSE);
	if (svWriteFile (keyfile, 0600)) {
		PLUGIN_WARN (IFCFG_PLUGIN_NAME, "    warning: could not update key file '%s'",
		             keyfile->fileName);
		svCloseFile (keyfile);
		goto error;
	}
	svCloseFile (keyfile);
	return;

error:
	/* Try setting the secret in the actual ifcfg */
	svSetValue (ifcfg, key, value, FALSE);
}

static gboolean
write_cert (NMSetting8021x *s_8021x,
            shvarFile *ifcfg,
            const char *setting_key,
            const char *ifcfg_key,
            const char *path_tag,
            const char *hash_tag,
            const char *prefix,
            gboolean is_pkcs12,
            gboolean *wrote,
            GError **error)
{
	const char *orig_hash, *orig_file;
	char *new_hash = NULL, *new_file = NULL;
	const GByteArray *cert = NULL;
	gboolean success = FALSE;
	GError *write_error = NULL;

	*wrote = FALSE;

	g_object_get (G_OBJECT (s_8021x), setting_key, &cert, NULL);
	if (!cert) {
		svSetValue (ifcfg, ifcfg_key, NULL, FALSE);
		return TRUE;
	}

	new_hash = utils_hash_byte_array (cert);
	if (!new_hash) {
		g_set_error (error, ifcfg_plugin_error_quark (), 0,
		             "Could not hash certificate data for %s / %s",
		             NM_SETTING_802_1X_SETTING_NAME, setting_key);
		return FALSE;
	}

	orig_hash = g_object_get_data (G_OBJECT (s_8021x), TAG_CA_CERT_HASH);
	orig_file = g_object_get_data (G_OBJECT (s_8021x), TAG_CA_CERT_PATH);

	if (!orig_hash || !orig_file || strcmp (new_hash, orig_hash)) {
		/* if the cert data has changed, or there wasn't a cert
		 * originally, write data out to the standard file.
		 */
		new_file = utils_cert_path (ifcfg->fileName, prefix, is_pkcs12 ? "p12" : "der");
		if (!new_file) {
			g_set_error (error, ifcfg_plugin_error_quark (), 0,
			             "Could not create file path for %s / %s",
			             NM_SETTING_802_1X_SETTING_NAME, setting_key);
			goto out;
		}

		if (!g_file_set_contents (new_file, (const char *) cert->data, cert->len, &write_error)) {
			g_set_error (error, ifcfg_plugin_error_quark (), 0,
			             "Could not write certificate for %s / %s: %s",
			             NM_SETTING_802_1X_SETTING_NAME, setting_key,
			             (write_error && write_error->message) ? write_error->message : "(unknown)");
			g_clear_error (&write_error);
			goto out;
		}
		*wrote = TRUE;

		svSetValue (ifcfg, ifcfg_key, new_file, FALSE);
		g_object_set_data_full (G_OBJECT (s_8021x), path_tag, new_file, g_free);
		new_file = NULL; /* g_object_set_data_full() took ownership */

		g_object_set_data_full (G_OBJECT (s_8021x), hash_tag, new_hash, g_free);
		new_hash = NULL; /* g_object_set_data_full() took ownership */
	} else {
		/* cert data hasn't changed */
		svSetValue (ifcfg, ifcfg_key, orig_file, FALSE);
	}
	success = TRUE;

out:
	g_free (new_hash);
	g_free (new_file);
	return success;
}

static gboolean
write_8021x_setting (NMConnection *connection, shvarFile *ifcfg, GError **error)
{
	NMSetting8021x *s_8021x;
	const char *value;
	char *tmp;
	gboolean success = FALSE, is_pkcs12 = FALSE, wrote;

	s_8021x = (NMSetting8021x *) nm_connection_get_setting (connection, NM_TYPE_SETTING_802_1X);
	if (!s_8021x)
		return TRUE;

	/* EAP method */
	if (nm_setting_802_1x_get_num_eap_methods (s_8021x)) {
		value = nm_setting_802_1x_get_eap_method (s_8021x, 0);
		if (value)
			tmp = g_ascii_strup (value, -1);
	}
	svSetValue (ifcfg, "IEEE_8021X_EAP_METHODS", tmp ? tmp : NULL, FALSE);
	g_free (tmp);

	svSetValue (ifcfg, "IEEE_8021X_IDENTITY",
	            nm_setting_802_1x_get_identity (s_8021x),
	            FALSE);

	svSetValue (ifcfg, "IEEE_8021X_ANON_IDENTITY",
	            nm_setting_802_1x_get_anonymous_identity (s_8021x),
	            FALSE);

	set_secret (ifcfg, "IEEE_8021X_PASSWORD", nm_setting_802_1x_get_password (s_8021x));
	            
	/* CA certificate */
	if (!write_cert (s_8021x, ifcfg,
	                 NM_SETTING_802_1X_CA_CERT,
	                 "IEEE_8021X_CA_CERT",
	                 TAG_CA_CERT_PATH,
	                 TAG_CA_CERT_HASH,
	                 "ca-cert",
	                 FALSE, &wrote,
	                 error))
		goto out;

	/* Private key */
	if (nm_setting_802_1x_get_private_key (s_8021x)) {
		if (nm_setting_802_1x_get_private_key_type (s_8021x) == NM_SETTING_802_1X_CK_TYPE_PKCS12)
			is_pkcs12 = TRUE;
	}
	if (!write_cert (s_8021x, ifcfg,
	                 NM_SETTING_802_1X_PRIVATE_KEY,
	                 "IEEE_8021X_PRIVATE_KEY",
	                 TAG_PRIVATE_KEY_PATH,
	                 TAG_PRIVATE_KEY_HASH,
	                 "private-key",
	                 is_pkcs12, &wrote,
	                 error))
		goto out;

	if (is_pkcs12) {
		svSetValue (ifcfg, "IEEE_8021X_PRIVATE_KEY_PASSWORD",
		            nm_setting_802_1x_get_private_key_password (s_8021x), FALSE);
	} else {
		/* Clear the private key password for non-pkcs12 private keys that
		 * we just wrote out, since it will be unencrypted.
		 */
		if (wrote)
			svSetValue (ifcfg, "IEEE_8021X_PRIVATE_KEY_PASSWORD", NULL, FALSE);

		/* Client certificate */
		if (!write_cert (s_8021x, ifcfg,
		                 NM_SETTING_802_1X_CA_CERT,
		                 "IEEE_8021X_CA_CERT",
		                 TAG_CA_CERT_PATH,
		                 TAG_CA_CERT_HASH,
		                 "ca-cert",
		                 FALSE, &wrote,
		                 error))
			goto out;
	}
	success = TRUE;

out:
	return success;
}

static gboolean
write_wireless_security_setting (NMConnection *connection,
                                 shvarFile *ifcfg,
                                 gboolean adhoc,
                                 gboolean *no_8021x,
                                 GError **error)
{
	NMSettingWirelessSecurity *s_wsec;
	const char *key_mgmt, *auth_alg, *key, *proto, *cipher, *psk;
	gboolean wep = FALSE, wpa = FALSE, first;
	char *tmp;
	guint32 i, num;
	GString *str;

	s_wsec = (NMSettingWirelessSecurity *) nm_connection_get_setting (connection, NM_TYPE_SETTING_WIRELESS_SECURITY);
	if (!s_wsec) {
		g_set_error (error, ifcfg_plugin_error_quark (), 0,
		             "Missing '%s' setting", NM_SETTING_WIRELESS_SECURITY_SETTING_NAME);
		return FALSE;
	}

	key_mgmt = nm_setting_wireless_security_get_key_mgmt (s_wsec);
	g_assert (key_mgmt);

	auth_alg = nm_setting_wireless_security_get_auth_alg (s_wsec);

	svSetValue (ifcfg, "DEFAULTKEY", NULL, FALSE);

	if (!strcmp (key_mgmt, "none")) {
		wep = TRUE;
		*no_8021x = TRUE;
	} else if (!strcmp (key_mgmt, "wpa-none") || !strcmp (key_mgmt, "wpa-psk")) {
		svSetValue (ifcfg, "KEY_MGMT", "WPA-PSK", FALSE);
		wpa = TRUE;
		*no_8021x = TRUE;
	} else if (!strcmp (key_mgmt, "ieee8021x")) {
		svSetValue (ifcfg, "KEY_MGMT", "IEEE8021X", FALSE);
	} else if (!strcmp (key_mgmt, "wpa-eap")) {
		svSetValue (ifcfg, "KEY_MGMT", "WPA-EAP", FALSE);
		wpa = TRUE;
	}

	svSetValue (ifcfg, "SECURITYMODE", NULL, FALSE);
	if (auth_alg) {
		if (!strcmp (auth_alg, "shared"))
			svSetValue (ifcfg, "SECURITYMODE", "restricted", FALSE);
		else if (!strcmp (auth_alg, "open"))
			svSetValue (ifcfg, "SECURITYMODE", "open", FALSE);
		else if (!strcmp (auth_alg, "leap")) {
			svSetValue (ifcfg, "SECURITYMODE", "leap", FALSE);
			svSetValue (ifcfg, "IEEE_8021X_USERNAME",
			            nm_setting_wireless_security_get_leap_username (s_wsec),
			            FALSE);
			set_secret (ifcfg, "IEEE_8021X_PASSWORD",
			            nm_setting_wireless_security_get_leap_password (s_wsec));
			*no_8021x = TRUE;
		}
	}

	if (wep) {
		/* Default WEP TX key index */
		tmp = g_strdup_printf ("%d", nm_setting_wireless_security_get_wep_tx_keyidx (s_wsec) + 1);
		svSetValue (ifcfg, "DEFAULTKEY", tmp, FALSE);
		g_free (tmp);
	}

	/* WEP keys */
	set_secret (ifcfg, "KEY", NULL); /* Clear any default key */
	for (i = 0; i < 4; i++) {
		key = nm_setting_wireless_security_get_wep_key (s_wsec, i);
		tmp = g_strdup_printf ("KEY%d", i + 1);
		set_secret (ifcfg, tmp, (wep && key) ? key : NULL);
		g_free (tmp);
	}

	/* WPA protos */
	svSetValue (ifcfg, "WPA_ALLOW_WPA", NULL, FALSE);
	svSetValue (ifcfg, "WPA_ALLOW_WPA2", NULL, FALSE);
	num = nm_setting_wireless_security_get_num_protos (s_wsec);
	for (i = 0; i < num; i++) {
		proto = nm_setting_wireless_security_get_proto (s_wsec, i);
		if (proto && !strcmp (proto, "wpa"))
			svSetValue (ifcfg, "WPA_ALLOW_WPA", "yes", FALSE);
		else if (proto && !strcmp (proto, "rsn"))
			svSetValue (ifcfg, "WPA_ALLOW_WPA2", "yes", FALSE);
	}

	/* WPA Pairwise ciphers */
	svSetValue (ifcfg, "CIPHER_PAIRWISE", NULL, FALSE);
	str = g_string_new (NULL);
	num = nm_setting_wireless_security_get_num_pairwise (s_wsec);
	for (i = 0, first = TRUE; i < num; i++) {
		if (!first)
			g_string_append_c (str, ' ');
		cipher = nm_setting_wireless_security_get_pairwise (s_wsec, i);
		tmp = g_ascii_strup (cipher, -1);
		g_string_append (str, tmp);
		g_free (tmp);
	}
	if (strlen (str->str))
		svSetValue (ifcfg, "CIPHER_PAIRWISE", str->str, FALSE);
	g_string_free (str, TRUE);

	/* WPA Group ciphers */
	svSetValue (ifcfg, "CIPHER_GROUP", NULL, FALSE);
	str = g_string_new (NULL);
	num = nm_setting_wireless_security_get_num_groups (s_wsec);
	for (i = 0, first = TRUE; i < num; i++) {
		if (!first)
			g_string_append_c (str, ' ');
		cipher = nm_setting_wireless_security_get_group (s_wsec, i);
		tmp = g_ascii_strup (cipher, -1);
		g_string_append (str, tmp);
		g_free (tmp);
	}
	if (strlen (str->str))
		svSetValue (ifcfg, "CIPHER_GROUP", str->str, FALSE);
	g_string_free (str, TRUE);

	/* WPA Passphrase */
	psk = nm_setting_wireless_security_get_psk (s_wsec);
	set_secret (ifcfg, "WPA_PSK", (wpa && psk) ? psk : NULL);

	return TRUE;
}

static gboolean
write_wireless_setting (NMConnection *connection,
                        shvarFile *ifcfg,
                        gboolean *no_8021x,
                        GError **error)
{
	NMSettingWireless *s_wireless;
	char *tmp, *tmp2;
	const GByteArray *ssid, *mac, *bssid;
	const char *mode;
	char buf[33];
	guint32 mtu, chan, i;
	gboolean adhoc = FALSE, hex_ssid = FALSE;

	s_wireless = (NMSettingWireless *) nm_connection_get_setting (connection, NM_TYPE_SETTING_WIRELESS);
	if (!s_wireless) {
		g_set_error (error, ifcfg_plugin_error_quark (), 0,
		             "Missing '%s' setting", NM_SETTING_WIRELESS_SETTING_NAME);
		return FALSE;
	}

	svSetValue (ifcfg, "HWADDR", NULL, FALSE);
	mac = nm_setting_wireless_get_mac_address (s_wireless);
	if (mac) {
		tmp = g_strdup_printf ("%02X:%02X:%02X:%02X:%02X:%02X",
		                       mac->data[0], mac->data[1], mac->data[2],
		                       mac->data[3], mac->data[4], mac->data[5]);
		svSetValue (ifcfg, "HWADDR", tmp, FALSE);
		g_free (tmp);
	}

	svSetValue (ifcfg, "MTU", NULL, FALSE);
	mtu = nm_setting_wireless_get_mtu (s_wireless);
	if (mtu) {
		tmp = g_strdup_printf ("%u", mtu);
		svSetValue (ifcfg, "MTU", tmp, FALSE);
		g_free (tmp);
	}

	ssid = nm_setting_wireless_get_ssid (s_wireless);
	if (!ssid) {
		g_set_error (error, ifcfg_plugin_error_quark (), 0,
		             "Missing SSID in '%s' setting", NM_SETTING_WIRELESS_SETTING_NAME);
		return FALSE;
	}
	if (!ssid->len || ssid->len > 32) {
		g_set_error (error, ifcfg_plugin_error_quark (), 0,
		             "Invalid SSID in '%s' setting", NM_SETTING_WIRELESS_SETTING_NAME);
		return FALSE;
	}

	/* If the SSID contains any non-printable characters, we need to use the
	 * hex notation of the SSID instead.
	 */
	for (i = 0; i < ssid->len; i++) {
		if (!isprint (ssid->data[i])) {
			hex_ssid = TRUE;
			break;
		}
	}

	if (hex_ssid) {
		GString *str;

		/* Hex SSIDs don't get quoted */
		str = g_string_sized_new (ssid->len * 2 + 3);
		g_string_append (str, "0x");
		for (i = 0; i < ssid->len; i++)
			g_string_append_printf (str, "%02X", ssid->data[i]);
		svSetValue (ifcfg, "ESSID", str->str, TRUE);
		g_string_free (str, TRUE);
	} else {
		/* Printable SSIDs get quoted */
		memset (buf, 0, sizeof (buf));
		memcpy (buf, ssid->data, ssid->len);
		tmp2 = svEscape (buf);
		tmp = g_strdup_printf ("\"%s\"", tmp2);
		svSetValue (ifcfg, "ESSID", tmp, TRUE);
		g_free (tmp2);
		g_free (tmp);
	}

	mode = nm_setting_wireless_get_mode (s_wireless);
	if (!mode || !strcmp (mode, "infrastructure")) {
		svSetValue (ifcfg, "MODE", "Managed", FALSE);
	} else if (!strcmp (mode, "adhoc")) {
		svSetValue (ifcfg, "MODE", "Ad-Hoc", FALSE);
		adhoc = TRUE;
	} else {
		g_set_error (error, ifcfg_plugin_error_quark (), 0,
		             "Invalid mode '%s' in '%s' setting",
		             mode, NM_SETTING_WIRELESS_SETTING_NAME);
		return FALSE;
	}

	svSetValue (ifcfg, "CHANNEL", NULL, FALSE);
	chan = nm_setting_wireless_get_channel (s_wireless);
	if (chan) {
		tmp = g_strdup_printf ("%u", chan);
		svSetValue (ifcfg, "CHANNEL", tmp, FALSE);
		g_free (tmp);
	}

	svSetValue (ifcfg, "BSSID", NULL, FALSE);
	bssid = nm_setting_wireless_get_bssid (s_wireless);
	if (bssid) {
		tmp = g_strdup_printf ("%02X:%02X:%02X:%02X:%02X:%02X",
		                       bssid->data[0], bssid->data[1], bssid->data[2],
		                       bssid->data[3], bssid->data[4], bssid->data[5]);
		svSetValue (ifcfg, "BSSID", tmp, FALSE);
		g_free (tmp);
	}

	if (nm_setting_wireless_get_security (s_wireless)) {
		if (!write_wireless_security_setting (connection, ifcfg, adhoc, no_8021x, error))
			return FALSE;
	}

	svSetValue (ifcfg, "TYPE", TYPE_WIRELESS, FALSE);

	return TRUE;
}

static gboolean
write_wired_setting (NMConnection *connection, shvarFile *ifcfg, GError **error)
{
	NMSettingWired *s_wired;
	const GByteArray *mac;
	char *tmp;
	guint32 mtu;

	s_wired = (NMSettingWired *) nm_connection_get_setting (connection, NM_TYPE_SETTING_WIRED);
	if (!s_wired) {
		g_set_error (error, ifcfg_plugin_error_quark (), 0,
		             "Missing '%s' setting", NM_SETTING_WIRED_SETTING_NAME);
		return FALSE;
	}

	mac = nm_setting_wired_get_mac_address (s_wired);
	if (mac) {
		tmp = g_strdup_printf ("%02X:%02X:%02X:%02X:%02X:%02X",
		                       mac->data[0], mac->data[1], mac->data[2],
		                       mac->data[3], mac->data[4], mac->data[5]);
		svSetValue (ifcfg, "HWADDR", tmp, FALSE);
		g_free (tmp);
	}

	mtu = nm_setting_wired_get_mtu (s_wired);
	if (mtu) {
		tmp = g_strdup_printf ("%u", mtu);
		svSetValue (ifcfg, "MTU", tmp, FALSE);
		g_free (tmp);
	}

	svSetValue (ifcfg, "TYPE", TYPE_ETHERNET, FALSE);

	return TRUE;
}

static void
write_connection_setting (NMSettingConnection *s_con, shvarFile *ifcfg)
{
	char *tmp;

	svSetValue (ifcfg, "NAME", nm_setting_connection_get_id (s_con), FALSE);
	svSetValue (ifcfg, "UUID", nm_setting_connection_get_uuid (s_con), FALSE);
	svSetValue (ifcfg, "ONBOOT",
	            nm_setting_connection_get_autoconnect (s_con) ? "yes" : "no",
	            FALSE);

	svSetValue (ifcfg, "LAST_CONNECT", NULL, FALSE);
	if (nm_setting_connection_get_timestamp (s_con)) {
		tmp = g_strdup_printf ("%llu", nm_setting_connection_get_timestamp (s_con));
		svSetValue (ifcfg, "LAST_CONNECT", tmp, FALSE);
		g_free (tmp);
	}
}

static gboolean
write_ip4_setting (NMConnection *connection, shvarFile *ifcfg, GError **error)
{
	NMSettingIP4Config *s_ip4;
	const char *value;
	char *addr_key, *prefix_key, *gw_key, *tmp;
	guint32 i, num;
	GString *searches;

	s_ip4 = (NMSettingIP4Config *) nm_connection_get_setting (connection, NM_TYPE_SETTING_IP4_CONFIG);
	if (!s_ip4) {
		g_set_error (error, ifcfg_plugin_error_quark (), 0,
		             "Missing '%s' setting", NM_SETTING_IP4_CONFIG_SETTING_NAME);
		return FALSE;
	}

	value = nm_setting_ip4_config_get_method (s_ip4);
	g_assert (value);
	if (!strcmp (value, NM_SETTING_IP4_CONFIG_METHOD_AUTO))
		svSetValue (ifcfg, "BOOTPROTO", "dhcp", FALSE);
	else if (!strcmp (value, NM_SETTING_IP4_CONFIG_METHOD_MANUAL))
		svSetValue (ifcfg, "BOOTPROTO", "none", FALSE);
	else if (!strcmp (value, NM_SETTING_IP4_CONFIG_METHOD_LINK_LOCAL))
		svSetValue (ifcfg, "BOOTPROTO", "autoip", FALSE);
	else if (!strcmp (value, NM_SETTING_IP4_CONFIG_METHOD_SHARED))
		svSetValue (ifcfg, "BOOTPROTO", "shared", FALSE);

	num = nm_setting_ip4_config_get_num_addresses (s_ip4);
	for (i = 0; i < 254; i++) {
		char buf[INET_ADDRSTRLEN + 1];
		NMIP4Address *addr;
		guint32 ip;

		if (i == 0) {
			addr_key = g_strdup ("IPADDR");
			prefix_key = g_strdup ("PREFIX");
			gw_key = g_strdup ("GATEWAY");
		} else {
			addr_key = g_strdup_printf ("IPADDR%d", i + 1);
			prefix_key = g_strdup_printf ("PREFIX%d", i + 1);
			gw_key = g_strdup_printf ("GATEWAY%d", i + 1);
		}

		if (i >= num) {
			svSetValue (ifcfg, addr_key, NULL, FALSE);
			svSetValue (ifcfg, prefix_key, NULL, FALSE);
			svSetValue (ifcfg, gw_key, NULL, FALSE);
		} else {
			addr = nm_setting_ip4_config_get_address (s_ip4, i);

			memset (buf, 0, sizeof (buf));
			ip = nm_ip4_address_get_address (addr);
			inet_ntop (AF_INET, (const void *) &ip, &buf[0], sizeof (buf));
			svSetValue (ifcfg, addr_key, &buf[0], FALSE);

			tmp = g_strdup_printf ("%u", nm_ip4_address_get_prefix (addr));
			svSetValue (ifcfg, prefix_key, tmp, FALSE);
			g_free (tmp);

			if (nm_ip4_address_get_gateway (addr)) {
				memset (buf, 0, sizeof (buf));
				ip = nm_ip4_address_get_gateway (addr);
				inet_ntop (AF_INET, (const void *) &ip, &buf[0], sizeof (buf));
				svSetValue (ifcfg, gw_key, &buf[0], FALSE);
			} else
				svSetValue (ifcfg, gw_key, NULL, FALSE);
		}

		g_free (addr_key);
		g_free (prefix_key);
		g_free (gw_key);
	}

	num = nm_setting_ip4_config_get_num_dns (s_ip4);
	for (i = 0; i < 254; i++) {
		char buf[INET_ADDRSTRLEN + 1];
		guint32 ip;

		addr_key = g_strdup_printf ("DNS%d", i + 1);

		if (i >= num)
			svSetValue (ifcfg, addr_key, NULL, FALSE);
		else {
			ip = nm_setting_ip4_config_get_dns (s_ip4, i);

			memset (buf, 0, sizeof (buf));
			inet_ntop (AF_INET, (const void *) &ip, &buf[0], sizeof (buf));
			svSetValue (ifcfg, addr_key, &buf[0], FALSE);
		}
		g_free (addr_key);
	}

	num = nm_setting_ip4_config_get_num_dns_searches (s_ip4);
	if (num > 0) {
		searches = g_string_new (NULL);
		for (i = 0; i < num; i++) {
			if (i > 0)
				g_string_append_c (searches, ' ');
			g_string_append (searches, nm_setting_ip4_config_get_dns_search (s_ip4, i));
		}
		svSetValue (ifcfg, "DOMAIN", searches->str, FALSE);
		g_string_free (searches, TRUE);
	} else
		svSetValue (ifcfg, "DOMAIN", NULL, FALSE);

	svSetValue (ifcfg, "PEERDNS", NULL, FALSE);
	svSetValue (ifcfg, "PEERROUTES", NULL, FALSE);
	svSetValue (ifcfg, "DHCP_HOSTNAME", NULL, FALSE);
	svSetValue (ifcfg, "DHCP_CLIENT_ID", NULL, FALSE);
	if (!strcmp (value, NM_SETTING_IP4_CONFIG_METHOD_AUTO)) {
		svSetValue (ifcfg, "PEERDNS",
		            nm_setting_ip4_config_get_ignore_auto_dns (s_ip4) ? "no" : "yes",
		            FALSE);

		svSetValue (ifcfg, "PEERROUTES",
		            nm_setting_ip4_config_get_ignore_auto_routes (s_ip4) ? "no" : "yes",
		            FALSE);

		value = nm_setting_ip4_config_get_dhcp_hostname (s_ip4);
		if (value)
			svSetValue (ifcfg, "DHCP_HOSTNAME", value, FALSE);

		value = nm_setting_ip4_config_get_dhcp_client_id (s_ip4);
		if (value)
			svSetValue (ifcfg, "DHCP_CLIENT_ID", value, FALSE);
	}


	return TRUE;
}

static char *
escape_id (const char *id)
{
	char *escaped = g_strdup (id);
	char *p = escaped;

	/* Escape random stuff */
	while (*p) {
		if (*p == ' ')
			*p = '_';
		else if (*p == '/')
			*p = '-';
		else if (*p == '\\')
			*p = '-';
		p++;
	}

	return escaped;
}

static gboolean
write_connection (NMConnection *connection,
                  const char *ifcfg_dir,
                  const char *filename,
                  const char *keyfile,
                  char **out_filename,
                  GError **error)
{
	NMSettingConnection *s_con;
	gboolean success = FALSE;
	shvarFile *ifcfg = NULL;
	char *ifcfg_name = NULL;
	const char *type;
	gboolean no_8021x = FALSE;

	s_con = NM_SETTING_CONNECTION (nm_connection_get_setting (connection, NM_TYPE_SETTING_CONNECTION));
	if (!s_con) {
		g_set_error (error, ifcfg_plugin_error_quark (), 0,
		             "Missing '%s' setting", NM_SETTING_CONNECTION_SETTING_NAME);
		return FALSE;
	}

	if (filename) {
		/* For existing connections, 'filename' should be full path to ifcfg file */
		ifcfg = svNewFile (filename);
		ifcfg_name = g_strdup (filename);
	} else {
		char *escaped;

		escaped = escape_id (nm_setting_connection_get_id (s_con));
		ifcfg_name = g_strdup_printf ("%s/ifcfg-%s", ifcfg_dir, escaped);
		ifcfg = svCreateFile (ifcfg_name);
		g_free (escaped);
	}

	if (!ifcfg) {
		g_set_error (error, ifcfg_plugin_error_quark (), 0,
		             "Failed to open/create ifcfg file '%s'", ifcfg_name);
		goto out;
	}

	type = nm_setting_connection_get_connection_type (s_con);
	if (!type) {
		g_set_error (error, ifcfg_plugin_error_quark (), 0,
		             "Missing connection type!");
		goto out;
	}

	if (!strcmp (type, NM_SETTING_WIRED_SETTING_NAME)) {
		if (!write_wired_setting (connection, ifcfg, error))
			goto out;
	} else if (!strcmp (type, NM_SETTING_WIRELESS_SETTING_NAME)) {
		if (!write_wireless_setting (connection, ifcfg, &no_8021x, error))
			goto out;
	} else {
		g_set_error (error, ifcfg_plugin_error_quark (), 0,
		             "Can't write connection type '%s'", type);
		goto out;
	}

	if (!no_8021x) {
		if (!write_8021x_setting (connection, ifcfg, error))
			goto out;
	}

	if (!write_ip4_setting (connection, ifcfg, error))
		goto out;

	write_connection_setting (s_con, ifcfg);

	if (svWriteFile (ifcfg, 0644)) {
		g_set_error (error, ifcfg_plugin_error_quark (), 0,
		             "Can't write connection '%s'", ifcfg->fileName);
		goto out;
	}

	svCloseFile (ifcfg);

	/* Only return the filename if this was a newly written ifcfg */
	if (out_filename && !filename)
		*out_filename = g_strdup (ifcfg_name);

	success = TRUE;

out:
	g_free (ifcfg_name);
	return success;
}

gboolean
writer_new_connection (NMConnection *connection,
                       const char *ifcfg_dir,
                       char **out_filename,
                       GError **error)
{
	return write_connection (connection, ifcfg_dir, NULL, NULL, out_filename, error);
}

gboolean
writer_update_connection (NMConnection *connection,
                          const char *ifcfg_dir,
                          const char *filename,
                          const char *keyfile,
                          GError **error)
{
	return write_connection (connection, ifcfg_dir, filename, keyfile, NULL, error);
}

