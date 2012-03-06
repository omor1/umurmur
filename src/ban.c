/* Copyright (C) 2009-2012, Martin Johansson <martin@fatbob.nu>
   Copyright (C) 2005-2012, Thorvald Natvig <thorvald@natvig.com>

   All rights reserved.

   Redistribution and use in source and binary forms, with or without
   modification, are permitted provided that the following conditions
   are met:

   - Redistributions of source code must retain the above copyright notice,
     this list of conditions and the following disclaimer.
   - Redistributions in binary form must reproduce the above copyright notice,
     this list of conditions and the following disclaimer in the documentation
     and/or other materials provided with the distribution.
   - Neither the name of the Developers nor the names of its contributors may
     be used to endorse or promote products derived from this software without
     specific prior written permission.

   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
   ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
   A PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE FOUNDATION OR
   CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
   EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
   PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
   PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
   LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
   NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
   SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#include <stdlib.h>
#include <time.h>
#include "log.h"
#include "list.h"
#include "ban.h"
#include "conf.h"
#include "ssl.h"

declare_list(banlist);
static int bancount; /* = 0 */
static int ban_duration;

void Ban_init(void)
{
	ban_duration = getIntConf(BAN_LENGTH);
	/* Read ban file here */
}

void Ban_deinit(void)
{
	/* Save banlist */
}
void Ban_UserBan(client_t *client, char *reason)
{
	ban_t *ban;
	char hexhash[41];

	ban = malloc(sizeof(ban_t));
	if (ban == NULL)
		Log_fatal("Out of memory");
	memset(ban, 0, sizeof(ban_t));
	
	memcpy(ban->hash, client->hash, 20);
	memcpy(&ban->address, &client->remote_tcp.sin_addr, sizeof(in_addr_t));
	ban->mask = 128;
	ban->reason = strdup(reason);
	ban->name = strdup(client->username);
	ban->time = time(NULL);
	ban->duration = ban_duration;
	Timer_init(&ban->startTime);
	list_add_tail(&ban->node, &banlist);
	bancount++;
	
	SSLi_hash2hex(ban->hash, hexhash);
	Log_info_client(client, "User kickbanned. Reason: '%s' Hash: %s IP: %s Banned for: %d seconds",
	                ban->name, ban->reason, hexhash, inet_ntoa(*((struct in_addr *)&ban->address)),
	                ban->duration);
}


void Ban_pruneBanned()
{
	struct dlist *itr;
	ban_t *ban;
	char hexhash[41];
	uint64_t bantime_long;
		
	list_iterate(itr, &banlist) {
		ban = list_get_entry(itr, ban_t, node);
		bantime_long = ban->duration * 1000000LL;
#ifdef DEBUG
		SSLi_hash2hex(ban->hash, hexhash);
		Log_debug("BL: User %s Reason: '%s' Hash: %s IP: %s Time left: %d",
		          ban->name, ban->reason, hexhash, inet_ntoa(*((struct in_addr *)&ban->address)),
		          bantime_long / 1000000LL - Timer_elapsed(&ban->startTime) / 1000000LL);
#endif
		/* Duration of 0 = forever */
		if (ban->duration != 0 && Timer_isElapsed(&ban->startTime, bantime_long)) {
			free(ban->name);
			free(ban->reason);
			list_del(&ban->node);
			free(ban);
			bancount--;
		}
	}
}

bool_t Ban_isBanned(client_t *client)
{
	struct dlist *itr;
	ban_t *ban;
	list_iterate(itr, &banlist) {
		ban = list_get_entry(itr, ban_t, node);
		if (memcmp(ban->hash, client->hash, 20) == 0) 
			return true;
	}
	return false;
	
}

bool_t Ban_isBannedAddr(in_addr_t *addr)
{
	struct dlist *itr;
	ban_t *ban;
	int mask;
	in_addr_t tempaddr1, tempaddr2;
	
	list_iterate(itr, &banlist) {
		ban = list_get_entry(itr, ban_t, node);
		mask = ban->mask - 96;
		if (mask < 32) { /* XXX - only ipv4 support */
			memcpy(&tempaddr1, addr, sizeof(in_addr_t));
			memcpy(&tempaddr2, &ban->address, sizeof(in_addr_t));
			tempaddr1 &= (2 ^ mask) - 1;
			tempaddr2 &= (2 ^ mask) - 1;
		}
		if (memcmp(&tempaddr1, &tempaddr2, sizeof(in_addr_t)) == 0) 
			return true;
	}
	return false;
}

int Ban_getBanCount(void)
{
	return bancount;
}

message_t *Ban_getBanList(void)
{
	int i = 0;
	struct dlist *itr;
	ban_t *ban;
	message_t *msg;
	struct tm timespec;
	char timestr[32];
	char hexhash[41];
	uint8_t address[16];
	
	msg = Msg_banList_create(bancount);
	list_iterate(itr, &banlist) {
		ban = list_get_entry(itr, ban_t, node);
		gmtime_r(&ban->time, &timespec);
		strftime(timestr, 32, "%Y-%m-%dT%H:%M:%S", &timespec);
		SSLi_hash2hex(ban->hash, hexhash);
		/* ipv4 representation as ipv6 address. */
		memset(address, 0, 16);
		memcpy(&address[12], &ban->address, 4);
		memset(&address[10], 0xff, 2); /* IPv4 */
		Msg_banList_addEntry(msg, i++, address, ban->mask, ban->name,
		                     hexhash, ban->reason, timestr, ban->duration);
	}
	return msg;
}

void Ban_clearBanList()
{
	ban_t *ban;
	struct dlist *itr, *save;
	list_iterate_safe(itr, save, &banlist) {
		ban = list_get_entry(itr, ban_t, node);
		free(ban->name);
		free(ban->reason);
		list_del(&ban->node);
		free(ban);
		bancount--;
	}
}

void Ban_putBanList(message_t *msg, int n_bans)
{
	int i = 0;
	struct tm timespec;
	ban_t *ban;
	char *hexhash, *name, *reason, *start;
	uint32_t duration, mask;
	uint8_t *address;
	
	for (i = 0; i < n_bans; i++) {
		Msg_banList_getEntry(msg, i, &address, &mask, &name, &hexhash, &reason, &start, &duration);
		ban = malloc(sizeof(ban_t));
		if (ban == NULL)
			Log_fatal("Out of memory");
		memset(ban, 0, sizeof(ban_t));
		SSLi_hex2hash(hexhash, ban->hash);
		memcpy(&ban->address, &address[12], 4);
		ban->mask = mask;
		ban->reason = strdup(reason);
		ban->name = strdup(name);
		strptime(start, "%Y-%m-%dT%H:%M:%S", &timespec);
		ban->time = mktime(&timespec);
		Timer_init(&ban->startTime);
		ban->duration = duration;
		list_add_tail(&ban->node, &banlist);
		bancount++;
	}
}
