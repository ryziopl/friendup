/*©mit**************************************************************************
*                                                                              *
* This file is part of FRIEND UNIFYING PLATFORM.                               *
* Copyright (c) Friend Software Labs AS. All rights reserved.                  *
*                                                                              *
* Licensed under the Source EULA. Please refer to the copy of the MIT License, *
* found in the file license_mit.txt.                                           *
*                                                                              *
*****************************************************************************©*/

#include "mobile_app.h"
#include "mobile_app_websocket.h"
#include <pthread.h>
#include <util/hashmap.h>
#include <system/json/jsmn.h>
#include <system/user/user.h>
#include <system/systembase.h>
#include <time.h>
#include <unistd.h>
#include <util/log/log.h>
#include <util/session_id.h>

#define MAX_CONNECTIONS_PER_USER 5
#define KEEPALIVE_TIME_s 180 //ping time (10 before)
#define ENABLE_MOBILE_APP_NOTIFICATION_TEST_SIGNAL 1

#define CKPT DEBUG("====== %d\n", __LINE__)

#if ENABLE_MOBILE_APP_NOTIFICATION_TEST_SIGNAL == 1
#include <signal.h>
void MobileAppTestSignalHandler(int signum);
#endif

//There is a need for two mappings, user->mobile connections and mobile connection -> user

typedef struct UserMobileAppConnectionsS UserMobileAppConnectionsT;
typedef struct MobileAppConnectionS MobileAppConnectionT;

//
// Mobile Application global structure
//

struct MobileAppConnectionS
{
	struct lws *websocket_ptr;
	void *user_data;
	char *session_id;
	time_t last_communication_timestamp;
	UserMobileAppConnectionsT *user_connections;
	unsigned int user_connection_index;
	mobile_app_status_t app_status;
	time_t most_recent_resume_timestamp;
	time_t most_recent_pause_timestamp;
	UserMobileApp *userMobileApp;
};

//
// single user connection structure
//

struct UserMobileAppConnectionsS
{
	char *username;
	FULONG userID;
	MobileAppConnectionT *connection[MAX_CONNECTIONS_PER_USER];
};

static Hashmap *_user_to_app_connections_map;
static Hashmap *_websocket_to_user_connections_map;

static pthread_mutex_t _session_removal_mutex; //used to avoid sending pings while a session is being removed
static pthread_t _ping_thread;

static void  MobileAppInit(void);
static int   MobileAppReplyError( struct lws *wsi, int error_code );
static int   MobileAppHandleLogin( struct lws *wsi, json_t *json );
static int   MobileAppAddNewUserConnection( struct lws *wsi, const char *username, void *udata );
static void* MobileAppPingThread( void *a );
static char* MobileAppGetWebsocketHash( struct lws *wsi );
static void  MobileAppRemoveAppConnection( UserMobileAppConnectionsT *connections, unsigned int connection_index );

/**
 * Write message to websocket
 *
 * @param mac pointer to mobile connection structure
 * @param msg pointer to string where message is stored
 * @param len length of message
 * @return number of bytes written to websocket
 */
static inline int WriteMessage( struct MobileAppConnectionS *mac, unsigned char *msg, int len )
{
	MobileAppNotif *man = (MobileAppNotif *) mac->user_data;
	if( man != NULL )
	{
		FQEntry *en = FCalloc( 1, sizeof( FQEntry ) );
		if( en != NULL )
		{
			DEBUG("Message added to queue\n");
			en->fq_Data = FMalloc( len+LWS_SEND_BUFFER_PRE_PADDING+LWS_SEND_BUFFER_POST_PADDING );
			memcpy( en->fq_Data+LWS_SEND_BUFFER_PRE_PADDING, msg, len );
			en->fq_Size = LWS_PRE+len;
	
			FQPushFIFO( &(man->man_Queue), en );
			lws_callback_on_writable( mac->websocket_ptr );
		}
	}
	return len;
}

/**
 * Initialize Mobile App
 */
static void MobileAppInit( void )
{
	DEBUG("Initializing mobile app module\n");

	_user_to_app_connections_map = HashmapNew();
	_websocket_to_user_connections_map = HashmapNew();

	pthread_mutex_init( &_session_removal_mutex, NULL );

	pthread_create( &_ping_thread, NULL/*default attributes*/, MobileAppPingThread, NULL/*extra args*/ );

#if ENABLE_MOBILE_APP_NOTIFICATION_TEST_SIGNAL == 1
	signal( SIGUSR1, MobileAppTestSignalHandler );
#endif
}

/**
 * Websocket callback delivered from a mobile app. This function is called from within websocket.c
 * when a frame is received.
 *
 * @param wsi pointer to main Websockets structure
 * @param reason type of received message (lws_callback_reasons type)
 * @param user user data
 * @param in message (array of chars)
 * @param len size of 'message'
 * @return 0 when success, otherwise error number
 */
int WebsocketAppCallback(struct lws *wsi, enum lws_callback_reasons reason, void *user __attribute__((unused)), void *in, size_t len )
{
	DEBUG("websocket callback, reason %d, len %zu, wsi %p\n", reason, len, wsi);
	MobileAppNotif *man = (MobileAppNotif *) user;

	if( reason == LWS_CALLBACK_PROTOCOL_INIT )
	{
		MobileAppInit();
		return 0;
	}
	
	DEBUG1("-------------------\nwebsocket_app_callback\n------------------\nreasond: %d\n", reason );

	if (reason == LWS_CALLBACK_CLOSED || reason == LWS_CALLBACK_WS_PEER_INITIATED_CLOSE)
	{
		char *websocket_hash = MobileAppGetWebsocketHash( wsi );
		MobileAppConnectionT *app_connection = HashmapGetData(_websocket_to_user_connections_map, websocket_hash);

		if (app_connection == NULL)
		{
			DEBUG("Websocket close - no user session found for this socket\n");
			return MobileAppReplyError(wsi, MOBILE_APP_ERR_NO_SESSION);
		}
		FRIEND_MUTEX_LOCK(&_session_removal_mutex);
		
		if( man != NULL && man->man_Initialized == 1 )
		{
			FQDeInitFree( &(man->man_Queue) );
		}
		
		//remove connection from user connnection struct
		UserMobileAppConnectionsT *user_connections = app_connection->user_connections;
		unsigned int connection_index = app_connection->user_connection_index;
		DEBUG("Removing connection %d for user <%s>\n", connection_index, user_connections->username);
		MobileAppRemoveAppConnection(user_connections, connection_index);

		HashmapRemove(_websocket_to_user_connections_map, websocket_hash);

		FRIEND_MUTEX_UNLOCK(&_session_removal_mutex);

		FFree(websocket_hash);
		return 0;
	}

	if (reason != LWS_CALLBACK_RECEIVE)
	{
		if( reason == LWS_CALLBACK_SERVER_WRITEABLE )
		{
			FQEntry *e = NULL;
			FRIEND_MUTEX_LOCK(&_session_removal_mutex);
			FQueue *q = &(man->man_Queue);
			
			DEBUG("[websocket_app_callback] WRITABLE CALLBACK, q %p\n", q );
			
			if( ( e = FQPop( q ) ) != NULL )
			{
				FRIEND_MUTEX_UNLOCK(&_session_removal_mutex);
				unsigned char *t = e->fq_Data+LWS_SEND_BUFFER_PRE_PADDING;
				t[ e->fq_Size+1 ] = 0;

				int res = lws_write( wsi, e->fq_Data+LWS_SEND_BUFFER_PRE_PADDING, e->fq_Size, LWS_WRITE_TEXT );
				DEBUG("[websocket_app_callback] message sent: %s len %d\n", e->fq_Data, res );

				int ret = lws_send_pipe_choked( wsi );
				
				if( e != NULL )
				{
					DEBUG("Release: %p\n", e->fq_Data );
					FFree( e->fq_Data );
					FFree( e );
				}
			}
			else
			{
				DEBUG("[websocket_app_callback] No message in queue\n");
				FRIEND_MUTEX_UNLOCK(&_session_removal_mutex);
			}
		}
		else
		{
			DEBUG("Unimplemented callback, reason %d\n", reason);
		}
		
		if( man != NULL && man->man_Queue.fq_First != NULL )
		{
			DEBUG("We have message to send, calling writable\n");
			lws_callback_on_writable( wsi );
		}
		
		return 0;
	}
	
	DEBUG("Initialize queue\n");
	char *data = (char*)in;
	if( man != NULL )
	{
		DEBUG(" Initialized %d\n", man->man_Initialized );
		if( man->man_Initialized == 0 )
		{
			memset( &(man->man_Queue), 0, sizeof( man->man_Queue ) );
			FQInit( &(man->man_Queue) );
			man->man_Initialized = 1;
			DEBUG("Queue initialized\n");
		}
	}
	else
	{
		FERROR("\n\n\n\nMAN is NULL\n\n\n\n");
	}

	if( len == 0 )
	{
		DEBUG("Empty websocket frame (reason %d)\n", reason);
		return 0;
	}

	DEBUG("Mobile app data: <%*s>\n", (unsigned int)len, data);

	jsmn_parser parser;
	jsmn_init(&parser);
	jsmntok_t tokens[16]; //should be enough

	int tokens_found = jsmn_parse(&parser, data, len, tokens, sizeof(tokens)/sizeof(tokens[0]));

	DEBUG("JSON tokens found %d\n", tokens_found);

	if (tokens_found < 1)
	{
		return MobileAppReplyError(wsi, MOBILE_APP_ERR_NO_JSON);
	}

	json_t json = { .string = data, .string_length = len, .token_count = tokens_found, .tokens = tokens };

	char *msg_type_string = json_get_element_string(&json, "t");

	//see if this websocket belongs to an existing connection
	char *websocket_hash = MobileAppGetWebsocketHash(wsi);
	MobileAppConnectionT *app_connection = HashmapGetData( _websocket_to_user_connections_map, websocket_hash );
	FFree(websocket_hash);

	if( msg_type_string )
	{
		//due to uniqueness of "t" field values only first letter has to be evaluated
		char first_type_letter = msg_type_string[0];
		DEBUG("Type letter <%c>\n", first_type_letter);

		if( first_type_letter == 'l'/*login*/)
		{
			return MobileAppHandleLogin(wsi, &json);
		}
		else
		{
			if (app_connection == NULL)
			{
				DEBUG("Session not found for this connection\n");
				return MobileAppReplyError(wsi, MOBILE_APP_ERR_NO_SESSION);
			}

			app_connection->last_communication_timestamp = time(NULL);

			switch (first_type_letter)
			{

			case 'p': 
				do
				{ //pause
					DEBUG("App is paused\n");
					app_connection->app_status = MOBILE_APP_STATUS_PAUSED;
					app_connection->most_recent_pause_timestamp = time(NULL);
					
					char response[LWS_PRE+64];
					strcpy(response+LWS_PRE, "{\"t\":\"pause\",\"status\":1}");
					DEBUG("Response: %s\n", response+LWS_PRE);
					lws_write(wsi, (unsigned char*)response+LWS_PRE, strlen(response+LWS_PRE), LWS_WRITE_TEXT);
					/*
					FQEntry *en = FCalloc( 1, sizeof( FQEntry ) );
					if( en != NULL )
					{
						en->fq_Data = FMalloc( 64+LWS_SEND_BUFFER_PRE_PADDING+LWS_SEND_BUFFER_POST_PADDING );
						memcpy( en->fq_Data+LWS_SEND_BUFFER_PRE_PADDING, "{\"t\":\"pause\",\"status\":1}", 24 );
						en->fq_Size = LWS_PRE+64;
						
						DEBUG("[websocket_app_callback] Msg to send: %s\n", en->fq_Data+LWS_SEND_BUFFER_PRE_PADDING );
			
						FRIEND_MUTEX_LOCK(&_session_removal_mutex);
						FQPushFIFO( &(man->man_Queue), en );
						FRIEND_MUTEX_UNLOCK(&_session_removal_mutex);
						lws_callback_on_writable( wsi );
					}
					*/
				}
				while (0);
			break;

			case 'r': 
				do
				{ //resume
					DEBUG("App is resumed\n");
					app_connection->app_status = MOBILE_APP_STATUS_RESUMED;
					app_connection->most_recent_resume_timestamp = time(NULL);
					
					char response[LWS_PRE+64];
					strcpy(response+LWS_PRE, "{\"t\":\"resume\",\"status\":1}");
					DEBUG("Response: %s\n", response+LWS_PRE);
					lws_write(wsi, (unsigned char*)response+LWS_PRE, strlen(response+LWS_PRE), LWS_WRITE_TEXT);
					/*
					FQEntry *en = FCalloc( 1, sizeof( FQEntry ) );
					if( en != NULL )
					{
						en->fq_Data = FMalloc( 64+LWS_SEND_BUFFER_PRE_PADDING+LWS_SEND_BUFFER_POST_PADDING );
						memcpy( en->fq_Data+LWS_SEND_BUFFER_PRE_PADDING, "{\"t\":\"resume\",\"status\":1}", 25 );
						en->fq_Size = LWS_PRE+64;
						
						DEBUG("[websocket_app_callback] Msg to send1: %s\n", en->fq_Data+LWS_SEND_BUFFER_PRE_PADDING );
			
						FRIEND_MUTEX_LOCK(&_session_removal_mutex);
						FQPushFIFO( &(man->man_Queue), en );
						FRIEND_MUTEX_UNLOCK(&_session_removal_mutex);
						lws_callback_on_writable( wsi );
					}
					*/
				}
				while (0);
			break;

			case 'e': //echo
				DEBUG("Echo from client\n");
				break;

			default:
				return MobileAppReplyError(wsi, MOBILE_APP_ERR_WRONG_TYPE);
			}

		}
	}
	else
	{
		return MobileAppReplyError(wsi, MOBILE_APP_ERR_NO_TYPE);
	}

	return 0; //should be unreachable
}

/**
 * Sends an error reply back to the app and closes the websocket.
 *
 * @param wsi pointer to a Websockets struct
 * @param error_code numerical value of the error code
 */
static int MobileAppReplyError(struct lws *wsi, int error_code)
{
	char response[LWS_PRE+32];
	snprintf(response+LWS_PRE, sizeof(response)-LWS_PRE, "{ \"t\":\"error\", \"status\":%d}", error_code);
	DEBUG("Error response: %s\n", response+LWS_PRE);

	DEBUG("WSI %p\n", wsi);
	lws_write(wsi, (unsigned char*)response+LWS_PRE, strlen(response+LWS_PRE), LWS_WRITE_TEXT);

	char *websocket_hash = MobileAppGetWebsocketHash( wsi );
	MobileAppConnectionT *app_connection = HashmapGetData(_websocket_to_user_connections_map, websocket_hash);
	FFree(websocket_hash);
	if( app_connection )
	{
		DEBUG("Cleaning up before closing socket\n");
		UserMobileAppConnectionsT *user_connections = app_connection->user_connections;
		unsigned int connection_index = app_connection->user_connection_index;
		DEBUG("Removing connection %d for user <%s>\n", connection_index, user_connections->username);
		MobileAppRemoveAppConnection(user_connections, connection_index);
	}

	return -1;
}

/**
 * User login handler
 *
 * @param wsi pointer to Websocket structure
 * @param json to json structure with login entry
 * @return 0 when success, otherwise error number
 */
static int MobileAppHandleLogin( struct lws *wsi, json_t *json )
{
	char *username_string = json_get_element_string( json, "user" );

	if( username_string == NULL )
	{
		return MobileAppReplyError(wsi, MOBILE_APP_ERR_LOGIN_NO_USERNAME);
	}

	char *password_string = json_get_element_string(json, "pass");

	if( password_string == NULL )
	{
		return MobileAppReplyError(wsi, MOBILE_APP_ERR_LOGIN_NO_PASSWORD);
	}

	//step 3 - check if the username and password is correct
	DEBUG("Login attempt <%s> <%s>\n", username_string, password_string);

	unsigned long block_time = 0;
	User *user = UMGetUserByNameDB( SLIB->sl_UM, username_string);

	AuthMod *a = SLIB->AuthModuleGet( SLIB );

	if( a->CheckPassword(a, NULL/*no HTTP request*/, user, password_string, &block_time) == FALSE )
	{
		DEBUG("Check = false\n");
		return MobileAppReplyError( wsi, MOBILE_APP_ERR_LOGIN_INVALID_CREDENTIALS );
	}
	else
	{
		DEBUG("Check = true\n");
		return MobileAppAddNewUserConnection( wsi, username_string, user );
	}
}

/**
 * Mobile App Ping thread
 *
 * @param a pointer to thread data (not used)
 * @return NULL
 */
static void* MobileAppPingThread( void *a __attribute__((unused)) )
{
	pthread_detach( pthread_self() );
	DEBUG("App ping thread started\n");

	while (1)
	{
		DEBUG("Checking app communication times\n");

		int users_count = HashmapLength(_user_to_app_connections_map);
		bool check_okay = true;

		unsigned int index = 0;

		HashmapElement *element = NULL;
		while( (element = HashmapIterate(_user_to_app_connections_map, &index)) != NULL )
		{
			UserMobileAppConnectionsT *user_connections = element->data;
			if( user_connections == NULL )
			{
				//the hashmap was invalidated while we were reading it? let's try another ping session....
				check_okay = false;
				break;
			}

			pthread_mutex_lock(&_session_removal_mutex);
			//mutex is needed because a connection can be removed at any time within websocket_app_callback,
			//so a race condition would lead to null-pointers and stuff...

			//iterate through all user connections
			for( int i = 0; i < MAX_CONNECTIONS_PER_USER; i++ )
			{
				if (user_connections->connection[i])
				{ //see if a connection exists
					if( time(NULL) - user_connections->connection[i]->last_communication_timestamp > KEEPALIVE_TIME_s)
					{
						DEBUG("Client <%s> connection %d requires a ping\n", user_connections->username, i);

						//send ping
						//char request[LWS_PRE+64];
						//strcpy(request+LWS_PRE, "{\"t\":\"keepalive\",\"status\":1}");
						//DEBUG("Request: %s\n", request+LWS_PRE);
						//lws_write(user_connections->connection[i]->websocket_ptr, (unsigned char*)request+LWS_PRE, strlen(request+LWS_PRE), LWS_WRITE_TEXT);
						MobileAppNotif *man = (MobileAppNotif *) user_connections->connection[i]->user_data;
						if( man != NULL )
						{
							FQEntry *en = FCalloc( 1, sizeof( FQEntry ) );
							if( en != NULL )
							{
								en->fq_Data = FMalloc( 64+LWS_SEND_BUFFER_PRE_PADDING+LWS_SEND_BUFFER_POST_PADDING );
								memcpy( en->fq_Data+LWS_SEND_BUFFER_PRE_PADDING, "{\"t\":\"keepalive\",\"status\":1}", 28 );
								en->fq_Size = LWS_PRE+64;
			
								FQPushFIFO( &(man->man_Queue), en );
								lws_callback_on_writable( user_connections->connection[i]->websocket_ptr );
							}
						}
					}
				}
			} //end of user connection loops
			pthread_mutex_unlock(&_session_removal_mutex);
		} //end of users loop

		if (check_okay)
		{
			sleep(KEEPALIVE_TIME_s);
		}
	}

	pthread_exit(0);
	return NULL; //should not exit anyway
}

/**
 * Add user connectiono to global list
 *
 * @param wsi pointer to Websocket connection
 * @param username name of user which passed login
 * @param user_data pointer to user data
 * @return 0 when success, otherwise error number
 */
static int MobileAppAddNewUserConnection( struct lws *wsi, const char *username, void *user_data )
{
	char *session_id = session_id_generate();

	UserMobileAppConnectionsT *user_connections = HashmapGetData(_user_to_app_connections_map, username);

	if (user_connections == NULL){ //this user does not have any connections yet

		//create a new connections struct
		user_connections = FCalloc(sizeof(UserMobileAppConnectionsT), 1);

		if( user_connections == NULL )
		{
			DEBUG("Allocation failed\n");
			return MobileAppReplyError(wsi, MOBILE_APP_ERR_INTERNAL);
		}
		else
		{
			DEBUG("Creating new struct for user <%s>\n", username);
			char *permanent_username = FCalloc( strlen(username)+1, 1 ); //TODO: error handling
			strcpy( permanent_username, username );
			user_connections->username = permanent_username;
			
			//
			// we must also attach UserID to User. This functionality will allow FC to find user by ID
			
			SQLLibrary *sqllib  = SLIB->LibrarySQLGet( SLIB );

			user_connections->userID = -1;
			if( sqllib != NULL )
			{
				char *qery = FMalloc( 1048 );
				qery[ 1024 ] = 0;
				sqllib->SNPrintF( sqllib, qery, 1024, "SELECT UserID FROM FUser WHERE `Name`=\"%s\"", username );
				void *res = sqllib->Query( sqllib, qery );
				if( res != NULL )
				{
					char **row;
					if( ( row = sqllib->FetchRow( sqllib, res ) ) )
					{
						if( row[ 0 ] != NULL )
						{
							char *end;
							user_connections->userID = strtoul( row[0], &end, 0 );
						}
					}
					sqllib->FreeResult( sqllib, res );
				}
				SLIB->LibrarySQLDrop( SLIB, sqllib );
				FFree( qery );
			}

			//FIXME: check the deallocation order for permanent_username as it is held both
			//by our internal sturcts and within hashmap structs

			//add the new connections struct to global users' connections map
			if( HashmapPut(_user_to_app_connections_map, permanent_username, user_connections) != MAP_OK )
			{
				DEBUG("Could not add new struct of user <%s> to global map\n", username);

				FFree(user_connections);
				return MobileAppReplyError(wsi, MOBILE_APP_ERR_INTERNAL);
			}
		}
	}

	//create struct holding this connection
	MobileAppConnectionT *new_connection = FCalloc(sizeof(MobileAppConnectionT), 1);

	new_connection->session_id = session_id;
	new_connection->last_communication_timestamp = time(NULL);
	new_connection->websocket_ptr = wsi;

	//add this struct to user connections struct
	int connection_to_replace_index = -1;
	for( int i = 0; i < MAX_CONNECTIONS_PER_USER; i++ )
	{
		if( user_connections->connection[i] == NULL )
		{ //got empty slot
			connection_to_replace_index = i;
			DEBUG("Will use slot %d for this connection\n", connection_to_replace_index);
			break;
		}
	}

	if( connection_to_replace_index == -1 )
	{ //no empty slots found - drop the oldest connection

		connection_to_replace_index = 0;
		unsigned int oldest_timestamp = user_connections->connection[0]->last_communication_timestamp;

		for( int i = 1; i < MAX_CONNECTIONS_PER_USER; i++ )
		{
			if( user_connections->connection[i] == NULL )
			{
				if( user_connections->connection[i]->last_communication_timestamp < oldest_timestamp )
				{
					oldest_timestamp = user_connections->connection[i]->last_communication_timestamp;
					connection_to_replace_index = i;
					DEBUG("Will drop old connection from slot %d (last comm %d)\n", connection_to_replace_index, oldest_timestamp);
				}
			}
		}
	}

	if( user_connections->connection[connection_to_replace_index] != NULL )
	{
		MobileAppRemoveAppConnection(user_connections, connection_to_replace_index);
	}

	DEBUG("Adding connection to slot %d\n", connection_to_replace_index);
	user_connections->connection[connection_to_replace_index] = new_connection;

	new_connection->user_data = user_data;
	new_connection->user_connections = user_connections; //provide back reference that will map websocket to a user
	new_connection->user_connection_index = connection_to_replace_index;

	char *websocket_hash = MobileAppGetWebsocketHash( wsi );

	HashmapPut(_websocket_to_user_connections_map, websocket_hash, new_connection); //TODO: error handling here
	//websocket_hash now belongs to the hashmap, don't free it here

	char response[LWS_PRE+64];
	int msgsize = snprintf(response+LWS_PRE, sizeof(response)-LWS_PRE, "{ \"t\":\"login\", \"status\":%d, \"keepalive\":%d}", 1, KEEPALIVE_TIME_s) + LWS_PRE;
	DEBUG("Response: %s\n", response+LWS_PRE);
	lws_write(wsi, (unsigned char*)response+LWS_PRE, msgsize, LWS_WRITE_TEXT);
	/*
	MobileAppNotif *man = (MobileAppNotif *) user_data;
	if( man != NULL )
	{
		FQEntry *en = FCalloc( 1, sizeof( FQEntry ) );
		if( en != NULL )
		{
			en->fq_Data = FMalloc( 64+LWS_SEND_BUFFER_PRE_PADDING+LWS_SEND_BUFFER_POST_PADDING );
			memcpy( en->fq_Data+LWS_SEND_BUFFER_PRE_PADDING, "{\"t\":\"keepalive\",\"status\":1}", 28 );
			en->fq_Size = LWS_PRE+64;
			DEBUG("Its time to send message: %s !\n", en->fq_Data+LWS_SEND_BUFFER_PRE_PADDING );
	
			FQPushFIFO( &(man->man_Queue), en );
			
			DEBUG("Added %p\n", man->man_Queue.fq_First );
			
			lws_callback_on_writable( wsi );
		}
	}
	else
	{
		FERROR("Cannot get access to userdata!\n");
	}
	*/
	return 0;
}

/**
 * Get websocket hash
 *
 * @param wsi pointer to Websocket connection
 * @return pointer to string with generated hash
 */
static char* MobileAppGetWebsocketHash( struct lws *wsi )
{
	/*FIXME: this is a dirty workaround for currently used hashmap module. It accepts
	 * only strings as keys, so we'll use the websocket pointer printed out as
	 * string for the key. Eventually there should be a hashmap implementation available
	 * that can use ints (or pointers) as keys!
	 */
	char *hash = FCalloc( 16, 1 );
	snprintf( hash, 16, "%p", wsi );
	return hash;
}

/**
 * Remove Connection from global list
 *
 * @param connections pointer to global list with connections
 * @param connection_index number of entry which will be removed from list
 */
static void  MobileAppRemoveAppConnection( UserMobileAppConnectionsT *connections, unsigned int connection_index )
{
	DEBUG("Freeing up connection from slot %d (last comm %ld)\n",
			connection_index,
			connections->connection[connection_index]->last_communication_timestamp);

	FFree(connections->connection[connection_index]->session_id);
	FFree(connections->connection[connection_index]);
	connections->connection[connection_index] = NULL;
}

/**
 * Notify user
 *
 * @param username pointer to string with user name
 * @param channel_id number of channel
 * @param title title of message which will be send to user
 * @param message message which will be send to user
 * @param notification_type type of notification
 * @param extra_string additional string which will be send to user
 * @return true when message was send
 */
int MobileAppNotifyUser( const char *username, const char *channel_id, const char *title, const char *message, MobileNotificationTypeT notification_type, const char *extra_string )
{
	UserMobileAppConnectionsT *user_connections = HashmapGetData( _user_to_app_connections_map, username );
	if( user_connections == NULL )
	{
		DEBUG("User <%s> does not have any app connections\n", username);
		return 1;
	}

	char *escaped_channel_id = json_escape_string(channel_id);
	char *escaped_title = json_escape_string(title);
	char *escaped_message = json_escape_string(message);
	char *escaped_extra_string = NULL;

	unsigned int required_length = strlen( escaped_channel_id ) + strlen( escaped_message ) + strlen( escaped_message ) + LWS_PRE + 128/*some slack*/;

	if( extra_string )
	{
		escaped_extra_string = json_escape_string( extra_string );
		required_length += strlen( escaped_extra_string );
	}

	char json_message[ required_length ];
	MobileManager *mm = SLIB->sl_MobileManager;

	if( extra_string )
	{ //TK-1039
		snprintf( json_message + LWS_PRE, sizeof(json_message)-LWS_PRE,
				"{\"t\":\"notify\",\"channel\":\"%s\",\"content\":\"%s\",\"title\":\"%s\",\"extra\":\"%s\"}", escaped_channel_id, escaped_message, escaped_title, escaped_extra_string );

		FFree( escaped_extra_string );
	}
	else
	{
		snprintf( json_message + LWS_PRE, sizeof(json_message)-LWS_PRE,
				"{\"t\":\"notify\",\"channel\":\"%s\",\"content\":\"%s\",\"title\":\"%s\",\"extra\":\"\"}", escaped_channel_id, escaped_message, escaped_title );
	}

	unsigned int json_message_length = strlen(json_message + LWS_PRE);

	FFree( escaped_channel_id );
	FFree( escaped_title );
	FFree( escaped_message );

	DEBUG("Send: <%s>\n", json_message + LWS_PRE);

	switch( notification_type )
	{
		case MN_force_all_devices:
		for( int i = 0; i < MAX_CONNECTIONS_PER_USER; i++ )
		{
			if( user_connections->connection[i] )
			{
				//WriteMessage( user_connections->connection[i], (unsigned char*)json_message, json_message_length );
				lws_write(user_connections->connection[i]->websocket_ptr,(unsigned char*)json_message+LWS_PRE,json_message_length,LWS_WRITE_TEXT);
			}
		}
		break;

		case MN_all_devices:
		for( int i = 0; i < MAX_CONNECTIONS_PER_USER; i++ )
		{
			if( user_connections->connection[i] && user_connections->connection[i]->app_status != MOBILE_APP_STATUS_RESUMED )
			{
				//WriteMessage( user_connections->connection[i], (unsigned char*)json_message, json_message_length );
				lws_write(user_connections->connection[i]->websocket_ptr,(unsigned char*)json_message+LWS_PRE,json_message_length,LWS_WRITE_TEXT);
			}
		}
		break;
		default: FERROR("**************** UNIMPLEMENTED %d\n", notification_type);
	}
	
	// message to user Android: "{\"t\":\"notify\",\"channel\":\"%s\",\"content\":\"%s\",\"title\":\"%s\"}"
	// message from example to APNS: /client.py '{"auth":"72e3e9ff5ac019cb41aed52c795d9f4c","action":"notify","payload":"hellooooo","sound":"default","token":"1f3b66d2d16e402b5235e1f6f703b7b2a7aacc265b5af526875551475a90e3fe","badge":1,"category":"whatever"}'
	
	char *json_message_ios;
	int json_message_ios_size = required_length+512;
	if( ( json_message_ios = FMalloc( json_message_ios_size ) ) != NULL )
	{
		MobileListEntry *mle = MobleManagerGetByUserIDDBPlatform( mm, user_connections->userID, MOBILE_APP_TYPE_IOS );
		while( mle != NULL )
		{
			snprintf( json_message_ios, json_message_ios_size, "{\"auth\":\"%s\",\"action\":\"notify\",\"payload\":\"%s\",\"sound\":\"default\",\"token\":\"%s\",\"badge\":1,\"category\":\"whatever\"}", SLIB->l_AppleKeyAPI, escaped_message, mle->mm_UMApp->uma_AppToken );
			
			WebsocketClientSendMessage( SLIB->l_APNSConnection, json_message_ios, json_message_ios_size );
			mle = (MobileListEntry *) mle->node.mln_Succ;
		}
		FFree( json_message_ios );
	}
	else
	{
		FERROR("Cannot allocate memory for MobileApp->user->ios message!\n");
	}
	
	return 0;
}

#if ENABLE_MOBILE_APP_NOTIFICATION_TEST_SIGNAL == 1

/**
 * Test signal handler
 *
 * @param signum signal number
 */
void MobileAppTestSignalHandler( int signum __attribute__((unused)))
{
	DEBUG("******************************* sigusr handler\n");

	static unsigned int counter = 0;

	counter++;

	char title[64];
	char message[64];
	sprintf( title, "Fancy title %d", counter );
	sprintf( message, "Fancy message %d", counter );

	int status = MobileAppNotifyUser( "fadmin",
			"test_app",
			title,
			message,
			MN_all_devices,
			NULL/*no extras*/);

	signal( SIGUSR1, MobileAppTestSignalHandler );
}
#endif
