#include "cJSON.h"
#include "utils.h"
#include "sbuf.structs.h"
#include "discord.h"
#include "discord.structs.h"
#include "media.h"
#include "media.structs.h"
#include "youtube_fetch.h"

#include <math.h>

#define DEFAULT_BOT_NAME "groov-in-c"
#define DEFAULT_BOT_PREFIX "-"
char *botname;
char *bottoken;
char *default_botprefix;

#define DEFAULT_LOG_LEVEL 0
#define LOG_FILE "output.log"
#define LOG_PY_FILE "py_scripts/parser.log"

#define RETRIES 10

sem_t play_cmd_mutex;

/* ------------------------------ Helper macros ----------------------------- */
#define CHECK_IN_VC(uobjp, guildid, dis, textchannelid)                        \
  if (!(has_user && (uobjp->vc_id[0] != 0) &&                                  \
        !strcmp(uobjp->guild_id, guildid))) {                                  \
    simple_send_msg(dis, "You must be in a voice channel!", textchannelid);    \
    return;                                                                    \
  }

#define CHECK_RIGHT_VC(wrong_vc, dis, textchannelid)                           \
  if (wrong_vc) {                                                              \
    simple_send_msg(                                                           \
        dis, "Please make sure I am joined or in the correct voice channel.",  \
        textchannelid);                                                        \
                                                                               \
    return;                                                                    \
  }

#define CHECK_MEDIA_INITIALIZED(vgt, dis, textchannelid)                       \
  if (!(vgt && vgt->media && vgt->media->initialized)) {                       \
    simple_send_msg(dis, "No song playing!", textchannelid);                   \
                                                                               \
    return;                                                                    \
  }

#define CHECK_PERMISSIONS(is_dj, dis, textchannelid)                           \
  if (!is_dj) {                                                                \
    simple_send_msg(dis, "You do not have permission to use this command!",    \
                    textchannelid);                                            \
    return;                                                                    \
  }

#define CHECK_SONG_PLAYING(vgt, dis, textchannelid)                            \
  if (!(vgt && vgt->media && vgt->media->playing)) {                           \
    simple_send_msg(dis, "Not currently playing a song!", textchannelid);      \
    return;                                                                    \
  }
#define CHECK_SKIPPABLE(vgt, dis, textchannelid)                               \
  if (!(vgt && vgt->media && vgt->media->skippable)) {                         \
    simple_send_msg(dis, "Not currently playing a song!", textchannelid);      \
    return;                                                                    \
  }

/* --------------------------------------------------------------------------*/
/*                              HELPER FUNCTIONS */
/* --------------------------------------------------------------------------*/

void simple_reconnect_send_raw(discord_t *dis, char *buffer, int buffer_len) {
  sem_wait(&(dis->https_writer_mutex));

  ssl_reconnect(dis->https_api_ssl, DISCORD_HOST, strlen(DISCORD_HOST),
                DISCORD_PORT, strlen(DISCORD_PORT));
  send_raw(dis->https_api_ssl, buffer, strlen(buffer));

  sem_post(&(dis->https_writer_mutex));
}

/* Simple sending message to discord
 *
 *
 *
 */
void simple_send_msg(discord_t *dis, char *text, char *textchannelid) {
  char message[4000];
  snprintf(message, 4000, DISCORD_API_POST_BODY_MSG_SIMPLE, text);
  char header[2000];
  snprintf(header, 2000, DISCORD_API_POST_MSG, textchannelid, bottoken,
           (int)strlen(message));
  char buffer[5000];
  snprintf(buffer, 5000, "%s\r\n\r\n%s\r\n\r\n", header, message);
  simple_reconnect_send_raw(dis, buffer, strlen(buffer));
}

/**
 * Simple sending a file
 * @param file Name of file to open
 * @param filename Display name of file sent on the message
 */
void simple_send_file(discord_t *dis, char *file, char *filename,
                      char *textchannelid) {

  FILE *fp = fopen(file, "rb");
  fseek(fp, 0, SEEK_END);
  long length = ftell(fp);
  size_t text_length = 0;
  fseek(fp, 0, SEEK_SET);

  char *text = malloc(length + 10);
  if (text) {
    text_length = fread(text, 1, length, fp);
  }
  text[text_length] = '\0';
  fclose(fp);

  char message[length + 500];
  char filename_section[strlen(filename) + 20];
  snprintf(filename_section, sizeof(filename_section), "filename=\"%s\"",
           filename);
  snprintf(message, text_length + 1000, POST_FORMDATA POST_FORMDATA_END, "file",
           filename_section, "Content-Type: \"plain/text\"", text);
  char header[2000];
  snprintf(header, 2000, DISCORD_API_POST_FILE, textchannelid, bottoken,
           (int)strlen(message));
  char buffer[strlen(header) + strlen(message) + 1000];
  snprintf(buffer, strlen(header) + strlen(message) + 1000,
           "%s\r\n\r\n%s\r\n\r\n", header, message);
  simple_reconnect_send_raw(dis, buffer, strlen(buffer));
  free(text);
}

void send_typing_indicator(discord_t *dis, char *textchannelid) {
  char header[1000];
  snprintf(header, sizeof(header), DISCORD_API_POST_TYPING, textchannelid,
           bottoken);
  char buffer[1100];
  snprintf(buffer, sizeof(buffer), "%s\r\n\r\n\r\n\r\n", header);
  simple_reconnect_send_raw(dis, buffer, strlen(buffer));
}

/* Helper Functions for sending strings in JSON
 *
 *  - these functions are necessary to escape out json string properly
 *
 */
void escape_http_newline(char *input, long unsigned in_size, char *output,
                         long unsigned out_size) {
  char *desc_after = input;
  char *desc_before = input;
  char *text_inputer = output;

  long unsigned cumulative_size = 0;
  while (desc_before < input + in_size) {
    desc_after = strchr(desc_before, '\n');

    if (desc_after) {
      *desc_after = 0;
    }

    strncpy(text_inputer, desc_before, out_size - cumulative_size);
    cumulative_size = cumulative_size + strlen(desc_before);

    if (cumulative_size + 2 >= out_size)
      break;

    if (desc_after) {
      output[cumulative_size] = '\\';
      output[cumulative_size + 1] = 'n';
      output[cumulative_size + 2] = 0;
      cumulative_size = cumulative_size + 2;
      desc_before = desc_after + 1;
      text_inputer = output + cumulative_size;
    } else {
      break;
    }
  }

  output[out_size - 1] = 0;
}
void escape_http_doublequote(char *input, long unsigned in_size, char *output,
                             long unsigned out_size) {
  char *desc_after = input;
  char *desc_before = input;
  char *text_inputer = output;

  long unsigned cumulative_size = 0;
  while (desc_before < input + in_size) {
    desc_after = strchr(desc_before, '"');

    if (desc_after) {
      *desc_after = 0;
    }

    strncpy(text_inputer, desc_before, out_size - cumulative_size);
    cumulative_size = cumulative_size + strlen(desc_before);

    if (cumulative_size + 2 >= out_size)
      break;

    if (desc_after) {
      output[cumulative_size] = '\\';
      output[cumulative_size + 1] = '"';
      output[cumulative_size + 2] = 0;
      cumulative_size = cumulative_size + 2;
      desc_before = desc_after + 1;
      text_inputer = output + cumulative_size;
    } else {
      break;
    }
  }

  output[out_size - 1] = 0;
}
void fix_string_ending(char *str) {
  int length = strlen(str);

  if (length == 0) {
    return;
  }

  unsigned char *working = ((unsigned char *)str) + length - 1;
  while (working != ((unsigned char *)str)) {
    if (((*working) & 0xC0) != 0x80) {
      break;
    }
    working--;
  }
  *working = 0;
}

/*  Voice reconnect callback
 *
 *  This function is called when bot needs to reconnect.
 *  Use this function to reconnect the MEDIA PLAYER
 */
void on_reconnect_voice(void *state, char *msg, unsigned long msg_len) {
  fprintf(stdout, "Reconnecting to media UDP channel\n");

  voice_gateway_t *vgt = (voice_gateway_t *)state;

  char secret_key[1000];
  char ip[100];
  char port[100];
  char *ssrc = "66666";

  sm_get(vgt->data_dictionary, DISCORD_VOICE_IP, ip, 100);
  sm_get(vgt->data_dictionary, DISCORD_VOICE_PORT, port, 100);
  sm_get(vgt->data_dictionary, DISCORD_VOICE_SECRET_KEY, secret_key, 1000);

  fprintf(stdout,
          "Media information: \nsecret_key:%s \nssrc:%s \nip:%s \nport:%s \n",
          secret_key, ssrc, ip, port);

  modify_player(vgt->media, secret_key, ssrc, ip, port, vgt->voice_udp_sockfd,
                "audiotmpfile.out", vgt);
}

/* Voice Gateway callback.
 *
 * Not much to do here
 *
 */
void on_message(void *state, char *msg, unsigned long msg_len) {
}

/* Connect bot/queue song in a thread.
 *
 * needs threaded model because connection needs to continue receiving websocket
 * while connecting...
 *
 */
struct play_cmd_obj {
  voice_gateway_t *vgt;
  discord_t *dis;
  user_vc_obj uobj;
  char *content;
  char *textchannelid;
  int insert_index;
};
void *threaded_play_cmd(void *ptr) {
  pthread_detach(pthread_self());

  struct play_cmd_obj *pobj = ptr;
  char *og_content = pobj->content;

  send_typing_indicator(pobj->dis, pobj->textchannelid);

  sem_wait(&(play_cmd_mutex));
  if (!(pobj->vgt)) {
    fprintf(stdout,
            "Voice not connected. Trying to connect to vgt. \nguild_id:%s "
            "\nvc_id:%s \n",
            pobj->uobj.guild_id, pobj->uobj.vc_id);

    pobj->vgt =
        connect_voice_gateway(pobj->dis, pobj->uobj.guild_id, pobj->uobj.vc_id,
                              on_message, on_reconnect_voice, 0);

    if (!(pobj->vgt)) {
      fprintf(stdout,
              "Failed to connect to voice. Reporting error in text channel.\n");
      simple_send_msg(pobj->dis,
                      "Failed to connect to voice channel. Make sure the bot "
                      "has permission to view voice channel.",
                      pobj->textchannelid);
      goto CLEANUP;
    }

    fprintf(stdout, "Successfully connected to voice!\n");

    char secret_key[1000];
    char ip[100];
    char port[100];
    char *ssrc = "66666";

    sm_get(pobj->vgt->data_dictionary, DISCORD_VOICE_IP, ip, 100);
    sm_get(pobj->vgt->data_dictionary, DISCORD_VOICE_PORT, port, 100);
    sm_get(pobj->vgt->data_dictionary, DISCORD_VOICE_SECRET_KEY, secret_key,
           1000);

    fprintf(stdout, "Initializing media player.\n");

    char filename[128] = {0};
    snprintf(filename, sizeof(filename) - 1, "audiotmpfile.%s.out",
             pobj->uobj.guild_id);
    pobj->vgt->media =
        start_player(secret_key, ssrc, ip, port, pobj->vgt->voice_udp_sockfd,
                     filename, pobj->vgt);
  }

  pobj->content += 3;

  fprintf(stdout, "Queueing song...\n");
  char title[200] = {0};
  int insert_queue_ret_error = 1;
  int queued_playlist = 0;
  if (!strncasecmp(pobj->content, "https://", 8)) {
    fprintf(stdout, "Query provided as URL.\n");
    if (!strncasecmp(pobj->content, "https://youtu.be/", 17) ||
        !strncasecmp(pobj->content, "https://www.youtube.com/watch?v=", 32)) {
      insert_queue_ret_error =
          insert_queue_ydl_query(pobj->vgt->media, pobj->content, title,
                                 sizeof(title), pobj->insert_index);
      if (insert_queue_ret_error) {
        fprintf(stdout, "ERROR: youtube-dl unable to queue song.\n");
      }
    } else if (!strncasecmp(pobj->content, "https://soundcloud.com/", 23)) {
      insert_queue_ret_error =
          insert_queue_soundcloud(pobj->vgt->media, pobj->content, title,
                                  sizeof(title), pobj->insert_index);
    } else {
      fprintf(stdout, "Invalid youtube url provided.\n");
    }

    // If a playlist is found
    fprintf(stdout, "Checking for playlist...\n");
    if (!strncasecmp(pobj->content, "https://www.youtube.com/", 24) &&
        ((strstr(pobj->content, "&list=") != NULL) ||
         (strstr(pobj->content, "playlist") != NULL))) {
      fprintf(stdout, "Playlist found!\n");

      queued_playlist = 1;

      // Search for index
      int start_index = 0;
      char *substr;
      if ((substr = strstr(pobj->content, "&index=")) != NULL) {
        char buf[20];
        int i = 0;
        while ((substr[i + 7] != '&') && (substr[i + 7] != 0)) {
          buf[i] = substr[i + 7];
          i++;
        }
        buf[i] = '\0';

        fprintf(stdout, "Queue index parameter: %s\n", buf);

        start_index = atoi(buf) - 1;
      }
      // if it's a playlist link without video, start at index -1
      if (strstr(pobj->content, "playlist") != NULL) {
        start_index = -1;
      }

      fprintf(stdout, "Starting queue at index: %d\n", start_index);

      int playlist_err = 0;

      int song_count = pobj->vgt->media->song_queue.size - 1;

      char *playlist_title = malloc(sizeof(char) * 1024);
      for (int i = 0; i < RETRIES; i++) {
        playlist_err = fetch_playlist(
            pobj->content, start_index + 1, pobj->vgt->media,
            (insert_partial_ytp_callback_f)insert_queue_ytb_partial,
            playlist_title);

        // If pass or KEY_ERR (for invalid urls)
        if (playlist_err == 0 || playlist_err == 4) {
          break;
        }
      }
      int new_song_count = pobj->vgt->media->song_queue.size - song_count;

      char message[1024];

      if (playlist_err) {
        fprintf(stdout, "Playlist error code: %d\n", playlist_err);
        snprintf(message, 1024, "Invalid playlist! Is the list private?");
      } else {
        snprintf(message, 1024, "Queued `%d` songs from `%s`", new_song_count,
                 playlist_title);
      }

      free(playlist_title);

      simple_send_msg(pobj->dis, message, pobj->textchannelid);
    }
  } else {
    fprintf(stdout, "Query provided as a search token.\n");
    char youtube_dl_search_txt[2048];
    snprintf(youtube_dl_search_txt, 2048, "ytsearch1:%s", pobj->content);
    insert_queue_ret_error =
        insert_queue_ydl_query(pobj->vgt->media, youtube_dl_search_txt, title,
                               sizeof(title), pobj->insert_index);
    if (insert_queue_ret_error) {
      fprintf(stdout, "ERROR: youtube-dl unable to queue song.\n");
    }
  }

  if (!insert_queue_ret_error) {
    int queue_len = pobj->vgt->media->song_queue.size;
    int skipper_val;
    sem_getvalue(&(pobj->vgt->media->skipper), &skipper_val);

    if (!(queue_len == 0 || (queue_len == 1 && skipper_val > 0))) {
      char message[300];

      // escape out the text for json sending
      char text3[sizeof(title)];
      char text4[sizeof(title)];
      escape_http_newline(title, sizeof(title), text3, sizeof(title));
      escape_http_doublequote(text3, sizeof(text3), text4, sizeof(text4));

      snprintf(message, sizeof(message), "Queued song: %s", text4);
      simple_send_msg(pobj->dis, message, pobj->textchannelid);
    }
  } else if (!queued_playlist) {
    simple_send_msg(pobj->dis,
                    "Unable to queue song. Please double check the link or "
                    "whether video is age restricted.",
                    pobj->textchannelid);
  }

CLEANUP:

  sem_post(&(play_cmd_mutex));

  free(og_content);
  free(pobj->textchannelid);
  free(pobj);

  return NULL;
}

/* This function handles collecting titles from the queue
 *
 */
void get_queue_callback(void *value, int len, void *state, int pos, int start,
                        int end) {
  char **array = state;
  youtube_page_object_t *ytobj = value;

  int index = 2 * (pos - start);

  array[index] = malloc(sizeof(ytobj->title));
  array[index + 1] = malloc(sizeof(ytobj->link));

  if (ytobj->title[0] == 0) {
    complete_youtube_object_fields(ytobj);
  }

  char text3[sizeof(ytobj->title)];
  char text4[sizeof(ytobj->title)];

  escape_http_newline(ytobj->title, sizeof(ytobj->title), text3,
                      sizeof(ytobj->title));
  escape_http_doublequote(text3, sizeof(text3), text4, sizeof(text4));

  memcpy(array[index], text4, sizeof(ytobj->title));
  memcpy(array[index + 1], ytobj->link, sizeof(ytobj->link));
}

/* set the prefix for each guild based on settings described in welcome channel
 * topic.
 *
 * bot needs to know what the prefix is...
 *
 */
void set_guild_config(discord_t *discord, char *msg, int msg_len) {
  int has_djroles_config = 0;

  cJSON *cjs = cJSON_ParseWithLength(msg, msg_len);
  cJSON *d_cjs = cJSON_GetObjectItem(cjs, "d");
  cJSON *gid_cjs = cJSON_GetObjectItem(d_cjs, "id");

  if (gid_cjs == NULL) {
    fprintf(stdout,
            "Error: cannot find guild id object in guild create response.\n");
    const char *error_ptr = cJSON_GetErrorPtr();
    if (error_ptr != NULL) {
      fprintf(stderr, "Error before: %s\n", error_ptr);
    }
  }
  fprintf(stdout, "Found guild:%s\n", gid_cjs->valuestring);

  cJSON *voice_states = cJSON_GetObjectItem(d_cjs, "voice_states");
  voice_states = voice_states->child;

  while (voice_states) {
    cJSON *uid, *cid;
    uid = cJSON_GetObjectItem(voice_states, "user_id");
    cid = cJSON_GetObjectItem(voice_states, "channel_id");

    user_vc_obj uobj;
    strcpy(uobj.user_id, uid->valuestring);
    strcpy(uobj.vc_id, cid->valuestring);
    strcpy(uobj.guild_id, gid_cjs->valuestring);

    sm_put(discord->user_vc_map, uobj.user_id, (char *)&uobj, sizeof(uobj));

    voice_states = voice_states->next;
  }
  fprintf(stdout, "  done parsing voice states...\n");

  char key[1000];
  cJSON *roles = cJSON_GetObjectItem(d_cjs, "roles");
  roles = roles->child;
  while (roles) {
    cJSON *name, *id;
    name = cJSON_GetObjectItem(roles, "name");
    id = cJSON_GetObjectItem(roles, "id");

    fprintf(stdout, "  found role: %s:%s\n", name->valuestring,
            id->valuestring);

    snprintf(key, sizeof(key), "%s%s%s", DISCORD_GATEWAY_ROLE_MAP_ITEM,
             gid_cjs->valuestring, name->valuestring);

    sm_put(discord->data_dictionary, key, id->valuestring,
           strlen(id->valuestring) + 1);

    roles = roles->next;
  }

  char botnamesearch[100];
  snprintf(botnamesearch, sizeof(botnamesearch), "@%s", botname);

  char *settings = strstr(msg, botnamesearch);
  if (!settings) {
    goto CLEANUP_GUILD_CONFIG;
  }

  settings = strchr(settings, ' ');
  if (!settings) {
    goto CLEANUP_GUILD_CONFIG;
  }
  settings++;

  char roleid[100];
  while (strncasecmp(settings, "end", 3)) {
    if (!strncasecmp(settings, "prefix", 6)) {
      char prefix = *(settings + 6);

      snprintf(key, sizeof(key), "%s%s", DISCORD_GATEWAY_GUILD_PREFIX_SETTING,
               gid_cjs->valuestring);

      sm_put(discord->data_dictionary, key, &prefix, sizeof(prefix));
    } else if (!strncasecmp(settings, "djroles", 7)) {
      has_djroles_config = 1;

      char *role = settings + 8;
      char *end;
      while (role[0] != ' ') {
        role++;
        end = strstr(role, "\\\"");
        *end = 0;

        snprintf(key, sizeof(key), "%s%s%s", DISCORD_GATEWAY_ROLE_MAP_ITEM,
                 gid_cjs->valuestring, role);

        int found =
            sm_get(discord->data_dictionary, key, roleid, sizeof(roleid));

        if (found) {
          snprintf(key, sizeof(key), "%s%s%s", DISCORD_GATEWAY_DJ_ROLES,
                   gid_cjs->valuestring, roleid);
          fprintf(stdout, "  djrole key: %s   ;%s\n", key, role);

          unsigned char allones = 0xFF;
          sm_put(discord->data_dictionary, key, (char *)&allones,
                 sizeof(allones));
        }

        *end = '\\';
        role = end + 2;
        if (role[0] == ' ')
          break;
        role += 2;
      }
    }

    settings = strchr(settings, ' ');
    if (!settings) {
      break;
    }
    settings++;
  }

CLEANUP_GUILD_CONFIG:

  if (!has_djroles_config) {
    fprintf(stdout, "  No DJ roles found, allowing everyone permission\n");

    snprintf(key, sizeof(key), "%s%s%s", DISCORD_GATEWAY_DJ_ROLES,
             gid_cjs->valuestring, DISCORD_GATEWAY_ROLE_EVERYONE);
    unsigned char allones = 0xFF;
    sm_put(discord->data_dictionary, key, (char *)&allones, sizeof(allones));
  }

  cJSON_Delete(cjs);
}

void leave_command(voice_gateway_t *vgt, discord_t *dis, user_vc_obj *uobjp,
                   char *guildid, char *textchannelid, int wrong_vc,
                   int has_user, int is_dj) {
  CHECK_IN_VC(uobjp, guildid, dis, textchannelid)
  CHECK_RIGHT_VC(wrong_vc, dis, textchannelid)
  CHECK_MEDIA_INITIALIZED(vgt, dis, textchannelid)
  CHECK_PERMISSIONS(is_dj, dis, textchannelid)

  // wait for song mutex
  sem_wait(&(vgt->media->insert_song_mutex));

  // skipping and quitting media player
  fprintf(stdout, "Quitting media player.\n");
  youtube_page_object_t ytpobj;
  sbuf_peek_end_value_copy(&(vgt->media->song_queue), &(ytpobj), sizeof(ytpobj),
                           0);
  sbuf_insert_front_value((&(vgt->media->song_queue)), &ytpobj, sizeof(ytpobj));
  sem_post(&(vgt->media->skipper));
  sem_post(&(vgt->media->quitter));

  // stop voice gateway listener thread to prevent potential segfault (maybe)
  fprintf(stdout, "Stopping voice heartbeat and listener.\n");
  sem_post(vgt->gateway_thread_exiter);
  pthread_cancel(vgt->heartbeat_tid);

  // delete voice gateway object from discord object and request a close
  char *ptr = 0;
  sm_put(dis->voice_gateway_map, uobjp->guild_id, (char *)&ptr, sizeof(void *));
  // send websocket close packet to vgt
  send_websocket(vgt->voice_ssl, "request close", strlen("request close"), 8);

  // send message to leave voice channel
  char msg[2000];
  snprintf(msg, 2000, DISCORD_GATEWAY_VOICE_LEAVE, uobjp->guild_id);
  sem_wait(&(dis->gateway_writer_mutex));
  send_websocket(dis->gateway_ssl, msg, strlen(msg), WEBSOCKET_OPCODE_MSG);
  sem_post(&(dis->gateway_writer_mutex));

  // clean up vgt data
  sm_delete(vgt->data_dictionary);
  disconnect_and_free_ssl(vgt->voice_ssl);
  free(vgt);

  // Done!
  fprintf(stdout, "Successfully left voice channel and cleaned up.\n");
}

void skip_command(voice_gateway_t *vgt, discord_t *dis, user_vc_obj *uobjp,
                  char *guildid, char *textchannelid, int wrong_vc,
                  int has_user, int is_dj) {

  CHECK_IN_VC(uobjp, guildid, dis, textchannelid)
  CHECK_RIGHT_VC(wrong_vc, dis, textchannelid)
  CHECK_MEDIA_INITIALIZED(vgt, dis, textchannelid)
  CHECK_PERMISSIONS(is_dj, dis, textchannelid)
  CHECK_SKIPPABLE(vgt, dis, textchannelid)

  sem_post(&(vgt->media->skipper));

  if (vgt->media->paused) {
    vgt->media->paused = 0;
    sem_post(&(vgt->media->pauser));
  }

  simple_send_msg(dis, "Skipped song!", textchannelid);
}

void desc_command(voice_gateway_t *vgt, discord_t *dis, user_vc_obj *uobjp,
                  char *guildid, char *textchannelid, int wrong_vc,
                  int has_user) {
  CHECK_IN_VC(uobjp, guildid, dis, textchannelid)
  CHECK_RIGHT_VC(wrong_vc, dis, textchannelid)
  CHECK_MEDIA_INITIALIZED(vgt, dis, textchannelid)
  CHECK_SONG_PLAYING(vgt, dis, textchannelid)

  char message[9500];
  youtube_page_object_t ytpobj;
  sbuf_peek_end_value_copy(&(vgt->media->song_queue), &(ytpobj), sizeof(ytpobj),
                           0);

  // check if youtube object is partially filled
  if (ytpobj.description[0] == 0) {
    complete_youtube_object_fields(&ytpobj);
  }

  // escape out descrption string
  char text[sizeof(ytpobj.description)];
  char text2[sizeof(ytpobj.description)];
  escape_http_newline(ytpobj.description, sizeof(ytpobj.description), text,
                      sizeof(ytpobj.description));
  escape_http_doublequote(text, sizeof(text), text2, sizeof(text2));
  fix_string_ending(text2);

  // escape out title string
  char text3[sizeof(ytpobj.title)];
  char text4[sizeof(ytpobj.title)];
  escape_http_newline(ytpobj.title, sizeof(ytpobj.title), text3,
                      sizeof(ytpobj.title));
  escape_http_doublequote(text3, sizeof(text3), text4, sizeof(text4));

  // form final message
  snprintf(message, 9500, DISCORD_API_POST_BODY_MSG_EMBED,
           "Now Playing:", text4, ytpobj.link, text2);

  // form sendable message
  char header[2000];
  snprintf(header, sizeof(header), DISCORD_API_POST_MSG, textchannelid,
           bottoken, (int)strlen(message));
  char buffer[13000];
  snprintf(buffer, sizeof(buffer), "%s\r\n\r\n%s\r\n\r\n", header, message);

  // send message
  simple_reconnect_send_raw(dis, buffer, strlen(buffer));
}

void now_playing_command(voice_gateway_t *vgt, discord_t *dis,
                         user_vc_obj *uobjp, char *guildid, char *textchannelid,
                         int wrong_vc, int has_user) {
  CHECK_IN_VC(uobjp, guildid, dis, textchannelid)
  CHECK_RIGHT_VC(wrong_vc, dis, textchannelid)
  CHECK_MEDIA_INITIALIZED(vgt, dis, textchannelid)
  CHECK_SONG_PLAYING(vgt, dis, textchannelid)

  char message[9500];
  youtube_page_object_t ytpobj;
  sbuf_peek_end_value_copy(&(vgt->media->song_queue), &(ytpobj), sizeof(ytpobj),
                           0);

  // escape out the text for json sending
  char text3[sizeof(ytpobj.title)];
  char text4[sizeof(ytpobj.title)];
  escape_http_newline(ytpobj.title, sizeof(ytpobj.title), text3,
                      sizeof(ytpobj.title));
  escape_http_doublequote(text3, sizeof(text3), text4, sizeof(text4));

  // get the current time to create progress bar
  fprintf(stdout, "song time: %f\n", vgt->media->current_song_time);
  long lapse = vgt->media->current_song_time + ytpobj.start_time_offset;

// create bar
#define barsize 30
  char bar[barsize] = {0};
  long progress = (barsize - 2) * lapse / (ytpobj.length_in_seconds);
  for (int i = 0; i < barsize - 2; i++) {
    if (i < progress)
      strcat(bar, "#");
    else
      strcat(bar, "-");
  }

  // print formatted time string
  char time_str[200] = {0};
  char hour[20] = {0};
  if (ytpobj.length_in_seconds / 3600 > 0) {
    snprintf(hour, sizeof(hour), "%02ld:", lapse / 3600);
  }
  snprintf(time_str, sizeof(time_str), "```%s%02ld:%02ld [%s] %s```", hour,
           (lapse / 60) % 60, lapse % 60, bar, ytpobj.duration);

  // print the final message
  snprintf(message, 9500, DISCORD_API_POST_BODY_MSG_EMBED,
           "Now Playing:", text4, ytpobj.link, time_str);

  // finalize message into sendable format
  char header[2000];
  snprintf(header, 2000, DISCORD_API_POST_MSG, textchannelid, bottoken,
           (int)strlen(message));
  char buffer[13000];
  snprintf(buffer, 13000, "%s\r\n\r\n%s\r\n\r\n", header, message);

  // send the message using ssl
  simple_reconnect_send_raw(dis, buffer, strlen(buffer));
}

void show_queue_command(voice_gateway_t *vgt, discord_t *dis,
                        user_vc_obj *uobjp, char *guildid, char *textchannelid,
                        char *content, int wrong_vc, int has_user) {
  CHECK_IN_VC(uobjp, guildid, dis, textchannelid)
  CHECK_RIGHT_VC(wrong_vc, dis, textchannelid)
  CHECK_MEDIA_INITIALIZED(vgt, dis, textchannelid)

  char message[9500];
#define QUEUELENGTH 10

  long int queue_page = strtol(content + 6, NULL, 10);
  if (queue_page == 0) {
    queue_page = 1;
  }
  queue_page -= 1;

  // get all the titles
  char *(title_arr[QUEUELENGTH * 2]) = {0};
  sbuf_iterate(&(vgt->media->song_queue), get_queue_callback, title_arr,
               queue_page * QUEUELENGTH, (queue_page + 1) * QUEUELENGTH - 1);

  // form the message
  char inner_message[5000] = {0};
  char temp_message[300];
  int queue_end = 0;
  int num_of_songs = vgt->media->song_queue.size - 1;
  long int num_of_queue_pages =
      (long int)ceil(((double)num_of_songs) / ((double)QUEUELENGTH));

  if (num_of_songs < 0) {
    num_of_songs = 0;
  }

  for (int x = 0; x < QUEUELENGTH * 2; x += 2) {
    int written_index = queue_page * QUEUELENGTH + (x / 2) + 1;
    if (written_index == 1 && title_arr[x]) {
      snprintf(temp_message, sizeof(temp_message), "```Now Playing: %.40s",
               title_arr[x]);
      if (strlen(title_arr[x]) > 40) {
        fix_string_ending(temp_message);
        strcat(temp_message, "...");
      }
      strcat(temp_message, "```");
      // strcat(temp_message, "\\n");
      strcat(temp_message, title_arr[x + 1]);
      strcat(temp_message, "\\n\\n");
      strcat(inner_message, temp_message);
    } else if (title_arr[x]) {
      snprintf(temp_message, sizeof(temp_message), "%d. %.40s", written_index,
               title_arr[x]);
      if (strlen(title_arr[x]) > 40) {
        fix_string_ending(temp_message);
        strcat(temp_message, "...");
      }
      strcat(temp_message, " [(link)](");
      strcat(temp_message, title_arr[x + 1]);
      strcat(temp_message, ")");
      strcat(temp_message, "\\n");
      strcat(inner_message, temp_message);
    } else {
      queue_end = 1;
    }
  }
  if (queue_end) {
    snprintf(temp_message, sizeof(temp_message), "\\n----End of Queue----\\n");
    strcat(inner_message, temp_message);
  }

  if (num_of_songs > 0) {
    queue_page = queue_page + 1;
  }

  snprintf(temp_message, sizeof(temp_message),
           "\\n Queue Page %ld of %ld. \\n Total %d songs in queue.",
           queue_page, num_of_queue_pages, num_of_songs);
  strcat(inner_message, temp_message);

  // cleanup
  for (int i = 0; i < QUEUELENGTH; i++) {
    free(title_arr[i]);
  }

  // form final message
  snprintf(message, 9500, DISCORD_API_POST_BODY_MSG_EMBED,
           "Song Queue:", "Up next on the playlist...", inner_message,
           "To see more songs, use \\\"queue [page number]\\\"");

  // create final message
  char header[2000];
  snprintf(header, 2000, DISCORD_API_POST_MSG, textchannelid, bottoken,
           (int)strlen(message));
  char buffer[13000];
  snprintf(buffer, 13000, "%s\r\n\r\n%s\r\n\r\n", header, message);

  // send message
  simple_reconnect_send_raw(dis, buffer, strlen(buffer));
}

void play_command(voice_gateway_t *vgt, discord_t *dis, user_vc_obj *uobjp,
                  char *guildid, char *textchannelid, char *content,
                  int wrong_vc, int has_user, int is_dj, int insert_index) {
  CHECK_IN_VC(uobjp, guildid, dis, textchannelid)
  CHECK_RIGHT_VC(wrong_vc, dis, textchannelid)
  CHECK_PERMISSIONS(is_dj, dis, textchannelid)

  struct play_cmd_obj *pobj = malloc(sizeof(struct play_cmd_obj));
  pobj->dis = dis;
  pobj->vgt = vgt;
  pobj->uobj = *uobjp;
  pobj->insert_index = insert_index;

  pobj->content = malloc(strlen(content) + 1);
  strcpy(pobj->content, content);
  pobj->textchannelid = malloc(strlen(textchannelid) + 1);
  strcpy(pobj->textchannelid, textchannelid);

  pthread_t tid;
  pthread_create(&tid, NULL, threaded_play_cmd, pobj);
}

void seek_command(voice_gateway_t *vgt, discord_t *dis, user_vc_obj *uobjp,
                  char *guildid, char *textchannelid, char *content,
                  int wrong_vc, int has_user, int is_dj) {
  CHECK_IN_VC(uobjp, guildid, dis, textchannelid)
  CHECK_RIGHT_VC(wrong_vc, dis, textchannelid)
  CHECK_MEDIA_INITIALIZED(vgt, dis, textchannelid)
  CHECK_PERMISSIONS(is_dj, dis, textchannelid)
  CHECK_SONG_PLAYING(vgt, dis, textchannelid)

  if (vgt->media->paused) {
    vgt->media->paused = 0;
    sem_post(&(vgt->media->pauser));
  }

  int length_in_seconds;
  char *duration = content + 5;
  char *time_sep = strchr(duration, ':');
  if (!time_sep) {
    length_in_seconds = strtol(duration, NULL, 10);
  } else {
    *time_sep = 0;
    time_sep++;
    char *time_sep2 = strchr(time_sep, ':');
    if (!time_sep2) {
      length_in_seconds =
          60 * strtol(duration, NULL, 10) + strtol(time_sep, NULL, 10);
    } else {
      *time_sep2 = 0;
      time_sep2++;
      length_in_seconds = 60 * 60 * strtol(duration, NULL, 10) +
                          60 * strtol(time_sep, NULL, 10) +
                          strtol(time_sep2, NULL, 10);
    }
  }

  fprintf(stdout, "Seeking to %d seconds.\n", length_in_seconds);

  seek_media_player(vgt->media, length_in_seconds);

  simple_send_msg(dis, "Seeked song!", textchannelid);
}

void shuffle_command(voice_gateway_t *vgt, discord_t *dis, user_vc_obj *uobjp,
                     char *guildid, char *textchannelid, int wrong_vc,
                     int has_user, int is_dj) {
  CHECK_IN_VC(uobjp, guildid, dis, textchannelid)
  CHECK_RIGHT_VC(wrong_vc, dis, textchannelid)
  CHECK_MEDIA_INITIALIZED(vgt, dis, textchannelid)
  CHECK_PERMISSIONS(is_dj, dis, textchannelid)

  send_typing_indicator(dis, textchannelid);
  shuffle_media_player(vgt->media);
  sleep(1); // in order to make sure typing indicator reaches first
  simple_send_msg(dis, "Shuffled Playlist!", textchannelid);
}

void clear_command(voice_gateway_t *vgt, discord_t *dis, user_vc_obj *uobjp,
                   char *guildid, char *textchannelid, int wrong_vc,
                   int has_user, int is_dj) {
  CHECK_IN_VC(uobjp, guildid, dis, textchannelid)
  CHECK_RIGHT_VC(wrong_vc, dis, textchannelid)
  CHECK_MEDIA_INITIALIZED(vgt, dis, textchannelid)
  CHECK_PERMISSIONS(is_dj, dis, textchannelid)

  send_typing_indicator(dis, textchannelid);
  clear_media_player(vgt->media);
  sleep(1); // in order to make sure typing indicator reaches first
  simple_send_msg(dis, "Cleared Playlist!", textchannelid);
}

void remove_command(voice_gateway_t *vgt, discord_t *dis, user_vc_obj *uobjp,
                    char *guildid, char *textchannelid, char *content,
                    int wrong_vc, int has_user, int is_dj) {
  CHECK_IN_VC(uobjp, guildid, dis, textchannelid)
  CHECK_RIGHT_VC(wrong_vc, dis, textchannelid)
  CHECK_MEDIA_INITIALIZED(vgt, dis, textchannelid)
  CHECK_PERMISSIONS(is_dj, dis, textchannelid)

  int remove_pos = strtol(content + 3, NULL, 10) - 1;

  if (remove_pos > 0)
    sbuf_remove_position_from_end(&(vgt->media->song_queue), remove_pos, NULL,
                                  0);

  simple_send_msg(dis, "Removed song!", textchannelid);
}

void pause_command(voice_gateway_t *vgt, discord_t *dis, user_vc_obj *uobjp,
                   char *guildid, char *textchannelid, int wrong_vc,
                   int has_user, int is_dj) {
  CHECK_IN_VC(uobjp, guildid, dis, textchannelid)
  CHECK_RIGHT_VC(wrong_vc, dis, textchannelid)
  CHECK_MEDIA_INITIALIZED(vgt, dis, textchannelid)
  CHECK_PERMISSIONS(is_dj, dis, textchannelid)

  if (!(vgt->media->paused)) {
    vgt->media->paused = 1;
    sem_wait(&(vgt->media->pauser));
    simple_send_msg(dis, "Paused!", textchannelid);
  } else {
    simple_send_msg(dis, "Not currently playing.", textchannelid);
  }
}

void resume_command(voice_gateway_t *vgt, discord_t *dis, user_vc_obj *uobjp,
                    char *guildid, char *textchannelid, int wrong_vc,
                    int has_user, int is_dj) {
  CHECK_IN_VC(uobjp, guildid, dis, textchannelid)
  CHECK_RIGHT_VC(wrong_vc, dis, textchannelid)
  CHECK_MEDIA_INITIALIZED(vgt, dis, textchannelid)
  CHECK_PERMISSIONS(is_dj, dis, textchannelid)

  if (vgt->media->paused) {
    vgt->media->paused = 0;
    sem_post(&(vgt->media->pauser));
    simple_send_msg(dis, "Resumed!", textchannelid);
  } else {
    simple_send_msg(dis, "Already playing.", textchannelid);
  }
}

void timestamps_command(voice_gateway_t *vgt, discord_t *dis,
                        user_vc_obj *uobjp, char *guildid, char *textchannelid,
                        int wrong_vc, int has_user, int is_dj) {

  CHECK_IN_VC(uobjp, guildid, dis, textchannelid)
  CHECK_RIGHT_VC(wrong_vc, dis, textchannelid)
  CHECK_MEDIA_INITIALIZED(vgt, dis, textchannelid)
  CHECK_SONG_PLAYING(vgt, dis, textchannelid)

  char message[9500];
  message[0] = 0;
  youtube_page_object_t ytpobj;
  sbuf_peek_end_value_copy(&(vgt->media->song_queue), &(ytpobj), sizeof(ytpobj),
                           0);

  // check if youtube object is partially filled
  if (ytpobj.description[0] == 0) {
    complete_youtube_object_fields(&ytpobj);
  }

  // escape out descirption string
  char text2[sizeof(ytpobj.description)];
  escape_http_doublequote(ytpobj.description, sizeof(ytpobj.description), text2,
                          sizeof(text2));
  fix_string_ending(text2);

  // escape out title string
  char text3[sizeof(ytpobj.title)];
  char text4[sizeof(ytpobj.title)];
  escape_http_newline(ytpobj.title, sizeof(ytpobj.title), text3,
                      sizeof(ytpobj.title));
  escape_http_doublequote(text3, sizeof(text3), text4, sizeof(text4));

  // Timestamps
  cJSON *timestamp_json_arr = cJSON_CreateArray();
  parse_description_timestamps(text2, timestamp_json_arr);

  char message_body[6000];
  message_body[0] = 0;

  for (int i = 0; i < cJSON_GetArraySize(timestamp_json_arr); i++) {
    cJSON *item = cJSON_GetArrayItem(timestamp_json_arr, i);
    char *label_json = cJSON_GetStringValue(cJSON_GetObjectItem(item, "label"));
    // cJSON *time_json = cJSON_GetObjectItem(item, "time");

    if (strlen(label_json) + strlen(message_body) + 30 >= 6000)
      break;

    char line_buf[1500];

    snprintf(line_buf, 1500, "\\n**%d.** `%s`", i + 1, label_json);

    strcat(message_body, line_buf);
  }

  if (!cJSON_GetArraySize(timestamp_json_arr))
    strcat(message_body, "\\nNo timestamps found.");

  cJSON_Delete(timestamp_json_arr);

  // print the final message
  snprintf(message, 9500, DISCORD_API_POST_BODY_MSG_EMBED, "Timestamps:", text4,
           message_body, "");

  // finalize message into sendable format
  char header[2000];
  snprintf(header, 2000, DISCORD_API_POST_MSG, textchannelid, bottoken,
           (int)strlen(message));
  char buffer[13000];
  snprintf(buffer, 13000, "%s\r\n\r\n%s\r\n\r\n", header, message);

  // send the message using ssl
  simple_reconnect_send_raw(dis, buffer, strlen(buffer));
}

void skip_timestamp_command(voice_gateway_t *vgt, discord_t *dis,
                            user_vc_obj *uobjp, char *guildid,
                            char *textchannelid, char *content, int wrong_vc,
                            int has_user, int is_dj) {
  CHECK_IN_VC(uobjp, guildid, dis, textchannelid)
  CHECK_RIGHT_VC(wrong_vc, dis, textchannelid)
  CHECK_MEDIA_INITIALIZED(vgt, dis, textchannelid)
  CHECK_PERMISSIONS(is_dj, dis, textchannelid)
  CHECK_SONG_PLAYING(vgt, dis, textchannelid)

  if (vgt->media->paused) {
    vgt->media->paused = 0;
    sem_post(&(vgt->media->pauser));
  }

  youtube_page_object_t ytpobj;
  sbuf_peek_end_value_copy(&(vgt->media->song_queue), &(ytpobj), sizeof(ytpobj),
                           0);

  // check if youtube object is partially filled
  if (ytpobj.description[0] == 0) {
    complete_youtube_object_fields(&ytpobj);
  }

  long time_elapsed = vgt->media->current_song_time + ytpobj.start_time_offset;

  // escape out descirption string
  char description_parsed[sizeof(ytpobj.description)];
  escape_http_doublequote(ytpobj.description, sizeof(ytpobj.description),
                          description_parsed, sizeof(description_parsed));
  fix_string_ending(description_parsed);

  // Timestamps
  cJSON *timestamp_json_arr = cJSON_CreateArray();
  parse_description_timestamps(description_parsed, timestamp_json_arr);

  int seek_time = 0;
  char label[1024];

  for (int i = 1; i < cJSON_GetArraySize(timestamp_json_arr); i++) {
    int cur_timestamp = (int)cJSON_GetNumberValue(cJSON_GetObjectItem(
        cJSON_GetArrayItem(timestamp_json_arr, i - 1), "timestamp"));
    int next_timestamp = (int)cJSON_GetNumberValue(cJSON_GetObjectItem(
        cJSON_GetArrayItem(timestamp_json_arr, i), "timestamp"));

    if (time_elapsed >= cur_timestamp && time_elapsed < next_timestamp) {
      seek_time = next_timestamp;

      char *cur_label = cJSON_GetStringValue(cJSON_GetObjectItem(
          cJSON_GetArrayItem(timestamp_json_arr, i), "label"));
      snprintf(label, 1023, "Skipped to timestamp #%d: **%s**", i + 1,
               cur_label);
      break;
    }
  }

  cJSON_Delete(timestamp_json_arr);

  if (seek_time == 0) {
    simple_send_msg(dis, "No timestamps to skip to!", textchannelid);
    return;
  }

  seek_media_player(vgt->media, seek_time);

  // print the final message
  simple_send_msg(dis, label, textchannelid);
}

void seek_timestamp_command(voice_gateway_t *vgt, discord_t *dis,
                            user_vc_obj *uobjp, char *guildid,
                            char *textchannelid, char *content, int wrong_vc,
                            int has_user, int is_dj) {
  CHECK_IN_VC(uobjp, guildid, dis, textchannelid)
  CHECK_RIGHT_VC(wrong_vc, dis, textchannelid)
  CHECK_MEDIA_INITIALIZED(vgt, dis, textchannelid)
  CHECK_PERMISSIONS(is_dj, dis, textchannelid)
  CHECK_SONG_PLAYING(vgt, dis, textchannelid)

  if (vgt->media->paused) {
    vgt->media->paused = 0;
    sem_post(&(vgt->media->pauser));
  }

  char *save_ptr;

  strtok_r(content, " ", &save_ptr);
  char *timestamp_str = strtok_r(NULL, " ", &save_ptr);

  int timestamp_no = timestamp_str ? atoi(timestamp_str) : 0;

  if (timestamp_no == 0) {
    simple_send_msg(dis,
                    "Please type a number starting from 1! Use the "
                    "`timestamps` command to check available timestamps.",
                    textchannelid);
    return;
  }

  youtube_page_object_t ytpobj;
  sbuf_peek_end_value_copy(&(vgt->media->song_queue), &(ytpobj), sizeof(ytpobj),
                           0);

  // check if youtube object is partially filled
  if (ytpobj.description[0] == 0) {
    complete_youtube_object_fields(&ytpobj);
  }

  // escape out descirption string
  char description_escaped[sizeof(ytpobj.description)];
  escape_http_doublequote(ytpobj.description, sizeof(ytpobj.description),
                          description_escaped, sizeof(description_escaped));
  fix_string_ending(description_escaped);

  // Timestamps
  cJSON *timestamp_json_arr = cJSON_CreateArray();
  parse_description_timestamps(description_escaped, timestamp_json_arr);

  int seek_time = -1;
  char label[1024];

  if (timestamp_no > cJSON_GetArraySize(timestamp_json_arr) ||
      timestamp_no <= 0) {

    simple_send_msg(dis, "Timestamp out of range!", textchannelid);
    cJSON_Delete(timestamp_json_arr);

    return;
  }

  seek_time = (int)cJSON_GetNumberValue(cJSON_GetObjectItem(
      cJSON_GetArrayItem(timestamp_json_arr, timestamp_no - 1), "timestamp"));
  char *cur_label = cJSON_GetStringValue(cJSON_GetObjectItem(
      cJSON_GetArrayItem(timestamp_json_arr, timestamp_no - 1), "label"));

  snprintf(label, 1023, "Seeked timestamp #%d: **%s**", timestamp_no,
           cur_label);

  cJSON_Delete(timestamp_json_arr);

  if (seek_time == -1) {
    simple_send_msg(dis, "No timestamps to seek to!", textchannelid);
    return;
  }

  seek_media_player(vgt->media, seek_time);

  // print the final message
  simple_send_msg(dis, label, textchannelid);
}

void help_command(voice_gateway_t *vgt, discord_t *dis, user_vc_obj *uobjp,
                  char *guildid, char *textchannelid, int wrong_vc,
                  int has_user) {

  char message[9500];

  snprintf(
      message, 9500, DISCORD_API_POST_BODY_MSG_EMBED,
      "List of Commands:", "groov-in-c",
      "Play music:                 `-p [youtube link or text to search youtube]`\\n\
      Play next (cut queue):      `-pn [youtube link or text]`\\n\
      Pause music:                `-pause`\\n\
      Resume after pause:         `-play`\\n\
      Skip music:                 `-skip`\\n\
      Show queue:                 `-queue`         (or `-queue [page number]`)\\n\
      Show current song:          `-np`\\n\
      Delete entry from queue:    `-r [entry number in queue]`\\n\
      Show youtube description:   `-desc`\\n\
      Make the bot leave:         `-leave`\\n\
      Clear queue:                `-clear`\\n\
      Shuffle queue:              `-shuffle`\\n\
      Seek music:                 `-seek [hour]:[mins]:[secs]`\\n\
          Example: `-seek 3:20` (goes to 3 minutes 20 seconds in the song)\\n\
      Show Timestamps:            `-timestamps`\\n\
      Seek to timestamp:          `-seekt [number]`\\n\
      Skip current song in video: `-skipt`\\n\
      List commands:              `-help`",
      "");
  // }

  // finalize message into sendable format
  char header[2000];
  snprintf(header, 2000, DISCORD_API_POST_MSG, textchannelid, bottoken,
           (int)strlen(message));
  char buffer[13000];
  snprintf(buffer, 13000, "%s\r\n\r\n%s\r\n\r\n", header, message);

  // send the message using ssl
  simple_reconnect_send_raw(dis, buffer, strlen(buffer));
}

/* ----------------------------- HELPER COMMANDS ---------------------------- */

// Sends the log file
void log_command(voice_gateway_t *vgt, discord_t *dis, user_vc_obj *uobjp,
                 char *guildid, char *textchannelid, int wrong_vc, int has_user,
                 int is_dj, char *file) {
  if (LOGGING) {
    simple_send_file(dis, file, file, textchannelid);
  } else {
    fprintf(stdout, "Please compile with -DLOGGING to enable logging");
  }
}

/* -------------------------------------------------------------------------- */
/*                                Main Funcions                               */
/* -------------------------------------------------------------------------- */

int check_user_dj_role(discord_t *dis, cJSON *cjs, char *guildid) {
  char key[200];
  snprintf(key, sizeof(key), "%s%s%s", DISCORD_GATEWAY_DJ_ROLES, guildid,
           DISCORD_GATEWAY_ROLE_EVERYONE);
  int found = sm_get(dis->data_dictionary, key, NULL, 0);
  if (found) {
    return 1;
  }

  cJSON *d_cjs = cJSON_GetObjectItem(cjs, "d");
  cJSON *member_cjs = cJSON_GetObjectItem(d_cjs, "member");
  cJSON *roles = cJSON_GetObjectItem(member_cjs, "roles");

  cJSON *role = roles->child;
  while (role) {
    snprintf(key, sizeof(key), "%s%s%s", DISCORD_GATEWAY_DJ_ROLES, guildid,
             role->valuestring);
    found = sm_get(dis->data_dictionary, key, NULL, 0);
    if (found) {
      return 1;
    }

    role = role->next;
  }

  return 0;
}

/* Gateway callback. handle all messages
 *
 * messages should be checked for proper tags...
 *
 */
void actually_do_shit(void *state, char *msg, unsigned long msg_len) {
  discord_t *dis = state;

  // fprintf(stdout, "msg: %s\n", msg);

  // handle adding members on startup
  if (strcasestr(msg, "\"GUILD_CREATE\"")) {
    set_guild_config(dis, msg, msg_len);
    return;
  }

  // debug disconnect gateway
  if (strstr(msg, "HelloFromTheOtherSide1234")) {
    send_websocket(dis->gateway_ssl, "request \"\"\" close",
                   strlen("request close"), 8);
    return;
  }

  // if someone sends a message
  if (strcasestr(msg, "\"MESSAGE_CREATE\"")) {
    cJSON *cjs = cJSON_ParseWithLength(msg, msg_len);
    cJSON *d_cjs = cJSON_GetObjectItem(cjs, "d");

    // get message content
    cJSON *content_cjs = cJSON_GetObjectItem(d_cjs, "content");
    char *content = content_cjs->valuestring;

    // get guild_id
    cJSON *guild_cjs = cJSON_GetObjectItem(d_cjs, "guild_id");
    char *guildid = guild_cjs->valuestring;

    // get the bot prefix for this guild
    char botprefix[10] = {0};
    botprefix[0] = default_botprefix[0];
    char key[200];
    snprintf(key, sizeof(key), "%s%s", DISCORD_GATEWAY_GUILD_PREFIX_SETTING,
             guildid);
    sm_get(dis->data_dictionary, key, botprefix, sizeof(botprefix));

    // handle bot prefix changing
    if ((content[0] == botprefix[0]) &&
        !strncasecmp(content + 1, "prefix", 6)) {
      botprefix[0] = *(content + 8);
      sm_put(dis->data_dictionary, key, &botprefix[0], sizeof(botprefix[0]));
      goto CLEANUP_CREATE_MESSAGE;
    }

    // get the user id of the person sending message
    cJSON *author_cjs = cJSON_GetObjectItem(d_cjs, "author");
    cJSON *uid_cjs = cJSON_GetObjectItem(author_cjs, "id");
    char *userid = uid_cjs->valuestring;

    // get channel id of the text channel where the message was sent
    cJSON *textchannelid_cjs = cJSON_GetObjectItem(d_cjs, "channel_id");
    char *textchannelid = textchannelid_cjs->valuestring;

    // check if user is a DJ
    int is_dj = check_user_dj_role(dis, cjs, guildid);

    // collect user info from USER INFO MAP
    user_vc_obj uobj;
    int has_user =
        sm_get(dis->user_vc_map, userid, (char *)&uobj, sizeof(uobj));

    // collect voice gateway from the gateway map
    voice_gateway_t *vgt = 0;
    sm_get(dis->voice_gateway_map, uobj.guild_id, (char *)&vgt, sizeof(void *));

    // check if user is in the wrong voice channel
    char bot_channel_id[100] = {0};
    int wrong_vc = 0;
    if (vgt) {
      int found =
          sm_get(vgt->data_dictionary, DISCORD_VOICE_STATE_UPDATE_CHANNEL_ID,
                 bot_channel_id, sizeof(bot_channel_id));
      wrong_vc = found && strcmp(bot_channel_id, uobj.vc_id);
    }

    // fprintf(stdout, "content: %s\n", content);

    if ((content[0] == botprefix[0])) {
      if (!strncasecmp(content + 1, "leave", 5)) {
        leave_command(vgt, dis, &uobj, guildid, textchannelid, wrong_vc,
                      has_user, is_dj);
      } else if (!strncasecmp(content + 1, "skipt", 5) ||
                 !strncasecmp(content + 1, "skip timestamp", 14)) {
        skip_timestamp_command(vgt, dis, &uobj, guildid, textchannelid, content,
                               wrong_vc, has_user, is_dj);
      } else if (!strncasecmp(content + 1, "seekt", 5)) {
        seek_timestamp_command(vgt, dis, &uobj, guildid, textchannelid, content,
                               wrong_vc, has_user, is_dj);
      } else if (!strncasecmp(content + 1, "skip", 4)) {
        skip_command(vgt, dis, &uobj, guildid, textchannelid, wrong_vc,
                     has_user, is_dj);
      } else if (!strncasecmp(content + 1, "desc", 4)) {
        desc_command(vgt, dis, &uobj, guildid, textchannelid, wrong_vc,
                     has_user);
      } else if (!strncasecmp(content + 1, "np", 2)) {
        now_playing_command(vgt, dis, &uobj, guildid, textchannelid, wrong_vc,
                            has_user);
      } else if (!strncasecmp(content + 1, "queue", 5)) {
        show_queue_command(vgt, dis, &uobj, guildid, textchannelid, content,
                           wrong_vc, has_user);
      } else if (!strncasecmp(content + 1, "p ", 2)) {
        play_command(vgt, dis, &uobj, guildid, textchannelid, content, wrong_vc,
                     has_user, is_dj, -1);
      } else if (!strncasecmp(content + 1, "seek ", 5)) {
        seek_command(vgt, dis, &uobj, guildid, textchannelid, content, wrong_vc,
                     has_user, is_dj);
      } else if (!strncasecmp(content + 1, "shuffle", 7)) {
        shuffle_command(vgt, dis, &uobj, guildid, textchannelid, wrong_vc,
                        has_user, is_dj);
      } else if (!strncasecmp(content + 1, "clear", 5)) {
        clear_command(vgt, dis, &uobj, guildid, textchannelid, wrong_vc,
                      has_user, is_dj);
      } else if (!strncasecmp(content + 1, "r ", 2)) {
        remove_command(vgt, dis, &uobj, guildid, textchannelid, content,
                       wrong_vc, has_user, is_dj);
      } else if (!strncasecmp(content + 1, "pause", 5)) {
        pause_command(vgt, dis, &uobj, guildid, textchannelid, wrong_vc,
                      has_user, is_dj);
      } else if (!strncasecmp(content + 1, "play ", 5)) {
        play_command(vgt, dis, &uobj, guildid, textchannelid, content + 3,
                     wrong_vc, has_user, is_dj, -1);
      } else if (!strncasecmp(content + 1, "play", 4)) {
        resume_command(vgt, dis, &uobj, guildid, textchannelid, wrong_vc,
                       has_user, is_dj);
      } else if (!strncasecmp(content + 1, "pn ", 3)) {
        play_command(vgt, dis, &uobj, guildid, textchannelid, content + 1,
                     wrong_vc, has_user, is_dj, 1);
      } else if (!strncasecmp(content + 1, "log", 3)) {
        log_command(vgt, dis, &uobj, guildid, textchannelid, wrong_vc, has_user,
                    is_dj, LOG_FILE);
      } else if (!strncasecmp(content + 1, "timestamps", 10)) {
        timestamps_command(vgt, dis, &uobj, guildid, textchannelid, wrong_vc,
                           has_user, is_dj);
      } else if (!strncasecmp(content + 1, "help", 4)) {
        help_command(vgt, dis, &uobj, guildid, textchannelid, wrong_vc,
                     has_user);
      }
    }

  CLEANUP_CREATE_MESSAGE:

    cJSON_Delete(cjs);
  }
}

int main(int argc, char **argv) {
  /* Logging */

  // Levels:
  // 0=LOG_TRACE 1=LOG_DEBUG 2=LOG_INFO 3=LOG_WARN 4=LOG_ERROR 5=LOG_FATAL
  char *log_level_env = getenv("LOG_LEVEL");
  int log_level = DEFAULT_LOG_LEVEL;
  if (log_level_env) {
    log_level = atoi(log_level_env);
  }

  log_set_level(log_level);

  FILE *fp = fopen(LOG_FILE, "w");
  log_add_fp(fp, log_level);

  log_info("Bot online!");

  // make sure fprintf stdout works on docker
  setbuf(stdout, NULL);

  sem_init(&(play_cmd_mutex), 0, 1);

  botname = getenv("BOT_NAME");
  if (!botname) {
    botname = DEFAULT_BOT_NAME;
  }

  default_botprefix = getenv("BOT_PREFIX");
  if (!default_botprefix) {
    default_botprefix = DEFAULT_BOT_PREFIX;
  }

  if (argc > 1) {
    bottoken = argv[1];
  } else {
    bottoken = getenv("TOKEN");
  }
  discord_t *discord = init_discord(bottoken, "641");

  fprintf(stdout, "Token: %s\n", bottoken);
  fprintf(stdout, "Bot default prefix: %c\n", default_botprefix[0]);

  char buf[100];
  sm_get(discord->data_dictionary, DISCORD_HOSTNAME_KEY, buf, 100);
  printf("%s\n", buf);

  set_gateway_callback(discord, actually_do_shit);
  connect_gateway(discord);

  while (1)
    sleep(100);

  free_discord(discord);
  fclose(fp);

  printf("exiting cleanly\n");
}
