#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <regex.h>
#include <curl/curl.h>
#include "cJSON.h"
#include "log.h"

/* -------------------------------- Lib curl -------------------------------- */
struct MemoryStruct {
  char *memory;
  size_t size;
};

/* --------------------------------- Helpers -------------------------------- */
int fetch_get(char *url, char **raw);

int trim_between(char *text, char const *start, char const *end);

typedef void (*insert_partial_ytp_callback_f)(void *media, char *id,
                                              char *title, char *duration,
                                              int length);

/* ----------------------------- Main functions ----------------------------- */

/**
 * @brief Fetches playlist and inserts into media list through callback.
 * Error codes:
 *  1: Curl error,
 *  2: Trim error,
 *  3: Json lengthText key error,
 *  4: Json playlist key error (likely due to invalid playlist ID)

 * @param url Video URL
 * @param start Start index
 * @param media Media object for callback
 * @param callback Insert partial youtube page callback
 * @param title Pointer to buffer where the title will be stored. Should contain
 atleast 200 characters
 * @return Error code
 */
int fetch_playlist(char *url, int start, void *media,
                   insert_partial_ytp_callback_f callback, char *title);

/**
 * Fetches description and escapes newlines and &
 * @param url Video URL
 * @param description Pointer to buffer where the description will be stored.
 * Should contain atleast 6000 characters
 */
int fetch_description_youtube_dl(char *url, char *description);

/**
 * Parses a non-newline escaped description and returns a JSON list of
 * timestamps and labels
 * @param description
 * @param timestamps_arr An empty cJSON array. Returns a list of objects in the
 * format [{"timestamp": int, "label": str}]. Must be freed with cJSON_Delete
 */
int parse_description_timestamps(char *description, cJSON *timestamps_arr);
