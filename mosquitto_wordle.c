/*
Copyright (c) 2022 Roger Light <roger@atchoo.org>

All rights reserved. This program and the accompanying materials
are made available under the terms of the Eclipse Public License 2.0
and Eclipse Distribution License v1.0 which accompany this distribution.

The Eclipse Public License is available at
   https://www.eclipse.org/legal/epl-2.0/
and the Eclipse Distribution License is available at
  http://www.eclipse.org/org/documents/edl-v10.php.

SPDX-License-Identifier: EPL-2.0 OR EDL-1.0

Contributors:
   Roger Light - initial implementation and documentation.
*/

#include <ctype.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <uthash.h>

#include "mosquitto_broker.h"
#include "mosquitto_plugin.h"
#include "mosquitto.h"
#include "mqtt_protocol.h"

#define PLUGIN_NAME "wordle"
#define PLUGIN_VERSION "1.0"

#ifndef UNUSED
#  define UNUSED(A) (void)(A)
#endif

MOSQUITTO_PLUGIN_DECLARE_VERSION(5);

#define USED_UNUSED 0
#define USED_INCORRECT 1
#define USED_HALFCORRECT 2
#define USED_CORRECT 3

struct client_table{
	UT_hash_handle hh;
	char attempts[6][5];
	uint8_t used[26];
	uint8_t attempt_number;
	bool won;
	char address[];
};

struct word_table{
	UT_hash_handle hh;
	char word[];
};

struct wordle_data{
	struct client_table *clients;
	struct word_table *word_table;
	char **word_list;
	int word_count;
	int current_word;
};

/* WORDLE_EPOCH tells us which word number we are currently on */
#define WORDLE_EPOCH 1645315200
static mosquitto_plugin_id_t *mosq_pid = NULL;
static struct wordle_data g_data;

static int calculate_current_word(void)
{
	time_t now = time(NULL) - WORDLE_EPOCH;
	return (int)((now/86400) % g_data.word_count);
}

static void clear_client_table(struct wordle_data *data)
{
	struct client_table *client, *client_tmp;

	HASH_ITER(hh, data->clients, client, client_tmp){
		HASH_DELETE(hh, data->clients, client);
		free(client);
	}
}

static void clear_word_list(struct wordle_data *data)
{
	struct word_table *word, *word_tmp;

	HASH_ITER(hh, data->word_table, word, word_tmp){
		HASH_DELETE(hh, data->word_table, word);
		free(word);
	}
	for(int i=0; i<data->word_count; i++){
		free(data->word_list[i]);
	}
	free(data->word_list);
}

static void publish(struct mosquitto *client, const char *msg)
{
	const char *id = mosquitto_client_id(client);
	mosquitto_broker_publish_copy(id, "wordle", (int)strlen(msg), msg, 0, false, NULL);
}


static bool is_letter_present(const char *haystack, char needle)
{
	for(int i=0; i<5; i++){
		if(tolower(needle) == haystack[i]){
			return true;
		}
	}
	return false;
}


static void add_matrix(char *response, struct client_table *client)
{
	for(int i=0; i<client->attempt_number+1; i++){
		for(int j=0; j<5; j++){
            if(client->attempts[i][j] == g_data.word_list[g_data.current_word][j]){
				strcat(response, "🟩");
			}else if(is_letter_present(g_data.word_list[g_data.current_word], client->attempts[i][j])){
				strcat(response, "🟨");
			}else{
				strcat(response, "⬜");
			}
		}
		strcat(response, "\n");
	}
}

static int acl_check_callback(int event, void *event_data, void *userdata)
{
	struct mosquitto_evt_acl_check *ed = event_data;
	struct client_table *client;
	struct word_table *word;
	int current_word;
	const char *address;
	char response[1000];

	UNUSED(event);
	UNUSED(userdata);

	/* We only process the "wordle" topic */
	if(strcmp(ed->topic, "wordle")) return MOSQ_ERR_PLUGIN_IGNORE;

	current_word = calculate_current_word();
	if(g_data.current_word != current_word){
		clear_client_table(&g_data);
		g_data.current_word = current_word;
	}

	/* Un/Subscriptions always succeed */
	if(ed->access == MOSQ_ACL_SUBSCRIBE || ed->access == MOSQ_ACL_UNSUBSCRIBE){
		return MOSQ_ERR_SUCCESS;
	}

	if(ed->access == MOSQ_ACL_WRITE){
		address = mosquitto_client_address(ed->client);

		HASH_FIND(hh, g_data.clients, address, strlen(address), client);
		if(!client){
			client = calloc(1, sizeof(struct client_table) + strlen(address) + 1);
			if(!client){
				publish(ed->client, "Internal error");
				return MOSQ_ERR_ACL_DENIED;
			}
			sprintf(client->address, "%s", address);
			HASH_ADD(hh, g_data.clients, address, strlen(client->address), client);
		}

		if(client->attempt_number == 6 || client->won){
			publish(ed->client, "Wait until tomorrow for the next word!");
			return MOSQ_ERR_ACL_DENIED;
		}

		/* Guessing attempt */
		if(ed->payloadlen != 5){
			publish(ed->client, "Word must be 5 letters long");
			return MOSQ_ERR_ACL_DENIED;
		}
		HASH_FIND(hh, g_data.word_table, ed->payload, ed->payloadlen, word);
		if(!word){
			publish(ed->client, "Word not found in list");
			return MOSQ_ERR_ACL_DENIED;
		}

		response[0] = '\0';
		sprintf(&response[strlen(response)], "%d/6: ", client->attempt_number+1);
		int correct = 0;
		for(int i=0; i<5; i++){
			client->attempts[client->attempt_number][i] = word->word[i];
			if(word->word[i] == g_data.word_list[g_data.current_word][i]){
				correct++;
				sprintf(&response[strlen(response)], "\e[32;1m%c\e[0m", tolower(word->word[i]));
				client->used[tolower(word->word[i])-'a'] = USED_CORRECT;
			}else if(is_letter_present(g_data.word_list[g_data.current_word], word->word[i])){
				sprintf(&response[strlen(response)], "\e[33;1m%c\e[0m", tolower(word->word[i]));
				if(client->used[tolower(word->word[i])-'a'] == USED_UNUSED){
					client->used[tolower(word->word[i])-'a'] = USED_HALFCORRECT;
				}
			}else{
				sprintf(&response[strlen(response)], "%c", tolower(word->word[i]));
				client->used[tolower(word->word[i])-'a'] = USED_INCORRECT;
			}
		}
		if(correct == 5){
			client->won = true;
			strcat(response, "\n\nWell done!\n\n");
			sprintf(&response[strlen(response)], "MQTT Wordle %d %d/6\n", g_data.current_word, client->attempt_number+1);
			add_matrix(response, client);
		}else{
			strcat(response, " (");
			for(int i=0; i<26; i++){
				if(client->used[i] == USED_CORRECT){
					sprintf(&response[strlen(response)], "\e[32;1m%c\e[0m", 'a'+i);
				}else if(client->used[i] == USED_HALFCORRECT){
					sprintf(&response[strlen(response)], "\e[33;1m%c\e[0m", 'a'+i);
				}else if(client->used[i] == USED_INCORRECT){
					sprintf(&response[strlen(response)], "\e[34m%c\e[0m", 'a'+i);
				}else{
					sprintf(&response[strlen(response)], "%c", 'a'+i);
				}
			}
			strcat(response, ")");
		}
		if(client->attempt_number == 5 && client->won == false){
			strcat(response, "\n\nOh dear!\n\n");
			sprintf(&response[strlen(response)], "MQTT Wordle %d x/6\n", g_data.current_word);
			add_matrix(response, client);
		}
		client->attempt_number++;
		publish(ed->client, response);

		return MOSQ_ERR_ACL_DENIED;
	}else{
		return MOSQ_ERR_SUCCESS;
	}
}


static int load_word_list(struct wordle_data *data)
{
	FILE *fptr;
	char line[20];
	int current_word;
	struct word_table *wt;

	fptr = fopen("/etc/mosquitto/words", "rb");
	if(!fptr) return 1;

	data->word_list = calloc(6000, sizeof(char *));
	if(!data->word_list){
		fclose(fptr);
		return MOSQ_ERR_NOMEM;
	}

	current_word = 0;
	while(!feof(fptr)){
		if(fgets(line, sizeof(line), fptr)){
			while(line[strlen(line)-1] == '\n'){
				line[strlen(line)-1] = '\0';
			}
			data->word_list[current_word] = strdup(line);
			if(!data->word_list[current_word]){
				fclose(fptr);
				return MOSQ_ERR_NOMEM;
			}
			current_word++;
			wt = calloc(1, sizeof(struct word_table) + strlen(line) + 1);
			if(!wt){
				fclose(fptr);
				return MOSQ_ERR_NOMEM;
			}
			strcpy(wt->word, line);
			HASH_ADD(hh, data->word_table, word, strlen(wt->word), wt);
		}
	}
	data->word_count= current_word;
	mosquitto_log_printf(MOSQ_LOG_INFO, "Wordle: %d words loaded", data->word_count);
	fclose(fptr);
	return MOSQ_ERR_SUCCESS;
}


int mosquitto_plugin_init(mosquitto_plugin_id_t *identifier, void **userdata, struct mosquitto_opt *opts, int opt_count)
{
	UNUSED(userdata);
	UNUSED(opts);
	UNUSED(opt_count);

	memset(&g_data, 0, sizeof(g_data));

	if(load_word_list(&g_data)){
		return MOSQ_ERR_UNKNOWN;
	}

	mosq_pid = identifier;
	mosquitto_plugin_set_info(identifier, PLUGIN_NAME, PLUGIN_VERSION);

	return mosquitto_callback_register(mosq_pid, MOSQ_EVT_ACL_CHECK, acl_check_callback, NULL, NULL);
}

int mosquitto_plugin_cleanup(void *userdata, struct mosquitto_opt *opts, int opt_count)
{
	UNUSED(userdata);
	UNUSED(opts);
	UNUSED(opt_count);

	clear_client_table(&g_data);
	clear_word_list(&g_data);

	return MOSQ_ERR_SUCCESS;
}
