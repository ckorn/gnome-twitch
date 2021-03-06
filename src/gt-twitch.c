#include "gt-twitch.h"
#include "gt-channel.h"
#include "gt-game.h"
#include "utils.h"
#include <libsoup/soup.h>
#include <glib/gprintf.h>
#include <glib.h>
#include <json-glib/json-glib.h>
#include <string.h>
#include <stdlib.h>

#define ACCESS_TOKEN_URI    "http://api.twitch.tv/api/channels/%s/access_token"
#define STREAM_PLAYLIST_URI "http://usher.twitch.tv/api/channel/hls/%s.m3u8?player=twitchweb&token=%s&sig=%s&allow_audio_only=true&allow_source=true&type=any&p=%d"
#define TOP_CHANNELS_URI    "https://api.twitch.tv/kraken/streams?limit=%d&offset=%d&game=%s"
#define TOP_GAMES_URI       "https://api.twitch.tv/kraken/games/top?limit=%d&offset=%d"
#define SEARCH_CHANNELS_URI "https://api.twitch.tv/kraken/search/streams?q=%s&limit=%d&offset=%d"
#define SEARCH_GAMES_URI    "https://api.twitch.tv/kraken/search/games?q=%s&type=suggest"
#define STREAMS_URI         "https://api.twitch.tv/kraken/streams/%s"
#define CHANNELS_URI        "https://api.twitch.tv/kraken/channels/%s"
#define CHAT_BADGES_URI     "https://api.twitch.tv/kraken/chat/%s/badges/"
#define TWITCH_EMOTE_URI    "http://static-cdn.jtvnw.net/emoticons/v1/%d/%d.0"
#define CHANNEL_INFO_URI    "http://api.twitch.tv/api/channels/%s/panels"
#define CHAT_SERVERS_URI    "https://api.twitch.tv/api/channels/%s/chat_properties"

#define STREAM_INFO "#EXT-X-STREAM-INF"

#define GT_TWITCH_ERROR_TOP_CHANNELS gt_spawn_twitch_top_channels_error_quark()
#define GT_TWITCH_ERROR_SEARCH_CHANNELS gt_spawn_twitch_search_channels_error_quark()

typedef struct
{
    SoupSession* soup;
} GtTwitchPrivate;

G_DEFINE_TYPE_WITH_PRIVATE(GtTwitch, gt_twitch,  G_TYPE_OBJECT)

enum
{
    PROP_0,
    NUM_PROPS
};

static GParamSpec* props[NUM_PROPS];

static GThreadPool* cache_update_pool;

typedef struct
{
    GtTwitch* twitch;

    gint64 int_1;
    gint64 int_2;
    gint64 int_3;

    gchar* str_1;
    gchar* str_2;
    gchar* str_3;

    gboolean bool_1;
    gboolean bool_2;
    gboolean bool_3;

    gpointer* gobject_ptr_1;
    gpointer* gobject_ptr_2;
    gpointer* gobject_ptr_3;
} GenericTaskData;

static GenericTaskData*
generic_task_data_new()
{
    return g_malloc0(sizeof(GenericTaskData));
}

static void
generic_task_data_free(GenericTaskData* data)
{
    g_free(data->str_1);
    g_free(data->str_2);
    g_free(data->str_3);
    g_clear_object(&data->gobject_ptr_1);
    g_clear_object(&data->gobject_ptr_2);
    g_clear_object(&data->gobject_ptr_3);
    g_free(data);
}

static void
cache_update_cb(gpointer data, gpointer user_data)
{
    GenericTaskData* task_data = data;

    GdkPixbuf* pic = gt_twitch_download_picture(task_data->twitch, task_data->str_1, task_data->int_1);
    if (pic)
    {
        gdk_pixbuf_save(pic, task_data->str_2, "jpeg", NULL, NULL);
        g_info("{GtTwitch} Updated cache entry for game '%s'", task_data->str_3);

        if (GT_IS_GAME(task_data->gobject_ptr_1))
            g_object_set(task_data->gobject_ptr_1, "preview", pic, NULL);
    }

    g_clear_object(&pic);
    generic_task_data_free(task_data);
}

static GtTwitchStreamAccessToken*
gt_twitch_stream_access_token_new()
{
    return g_new0(GtTwitchStreamAccessToken, 1);
}

void
gt_twitch_stream_access_token_free(GtTwitchStreamAccessToken* token)
{
    g_free(token->token);
    g_free(token->sig);
    g_free(token);
}

static GQuark
gt_spawn_twitch_top_channels_error_quark()
{
    return g_quark_from_static_string("gt-twitch-top-channels-error");
}

static GQuark
gt_spawn_twitch_search_channels_error_quark()
{
    return g_quark_from_static_string("gt-twitch-search-channels-error");
}

GtTwitch*
gt_twitch_new(void)
{
    return g_object_new(GT_TYPE_TWITCH,
                        NULL);
}

static void
finalize(GObject* object)
{
    GtTwitch* self = (GtTwitch*) object;
    GtTwitchPrivate* priv = gt_twitch_get_instance_private(self);

    G_OBJECT_CLASS(gt_twitch_parent_class)->finalize(object);
}

static void
get_property (GObject*    obj,
              guint       prop,
              GValue*     val,
              GParamSpec* pspec)
{
    GtTwitch* self = GT_TWITCH(obj);

    switch (prop)
    {
        default:
            G_OBJECT_WARN_INVALID_PROPERTY_ID(obj, prop, pspec);
    }
}

static void
set_property(GObject*      obj,
             guint         prop,
             const GValue* val,
             GParamSpec*   pspec)
{
    GtTwitch* self = GT_TWITCH(obj);

    switch (prop)
    {
        default:
            G_OBJECT_WARN_INVALID_PROPERTY_ID(obj, prop, pspec);
    }
}

static void
gt_twitch_class_init(GtTwitchClass* klass)
{
    GObjectClass* object_class = G_OBJECT_CLASS(klass);

    object_class->finalize = finalize;
    object_class->get_property = get_property;
    object_class->set_property = set_property;

    cache_update_pool = g_thread_pool_new((GFunc) cache_update_cb, NULL, 1, FALSE, NULL);
}

static void
gt_twitch_init(GtTwitch* self)
{
    GtTwitchPrivate* priv = gt_twitch_get_instance_private(self);

    priv->soup = soup_session_new();
}

static gboolean
send_message(GtTwitch* self, SoupMessage* msg)
{
    GtTwitchPrivate* priv = gt_twitch_get_instance_private(self);

    char* uri = soup_uri_to_string(soup_message_get_uri(msg), FALSE);

    g_info("{GtTwitch} Sending message to uri '%s'", uri);

    soup_session_send_message(priv->soup, msg);

    g_debug("{GtTwitch} Received code '%d' and response '%s'", msg->status_code, msg->response_body->data);

    /* g_print("\n\n%s\n\n", msg->response_body->data); */

    g_free(uri);

    return msg->status_code == SOUP_STATUS_OK;
}

static GDateTime*
parse_time(const gchar* time)
{
    GDateTime* ret = NULL;

    gint year, month, day, hour, min, sec;

    sscanf(time, "%d-%d-%dT%d:%d:%dZ", &year, &month, &day, &hour, &min, &sec);

    ret = g_date_time_new_utc(year, month, day, hour, min, sec);

    return ret;
}

static GList*
parse_playlist(const gchar* playlist)
{
    GList* ret = NULL;
    gchar** lines = g_strsplit(playlist, "\n", 0);

    for (gchar** l = lines; *l != NULL; l++)
    {
        if (strncmp(*l, STREAM_INFO, strlen(STREAM_INFO)) == 0)
        {
            GtTwitchStreamData* stream = g_malloc0(sizeof(GtTwitchStreamData));
            ret = g_list_append(ret, stream);

            gchar** values = g_strsplit(*l, ",", 0);

            for (gchar** m = values; *m != NULL; m++)
            {
                gchar** split = g_strsplit(*m, "=", 0);

                if (strcmp(*split, "BANDWIDTH") == 0)
                {
                    stream->bandwidth = atol(*(split+1));
                }
                else if (strcmp(*split, "RESOLUTION") == 0)
                {
                    gchar** res = g_strsplit(*(split+1), "x", 0);

                    stream->width = atoi(*res);
                    stream->height = atoi(*(res+1));

                    g_strfreev(res);
                }
                else if (strcmp(*split, "VIDEO") == 0)
                {
                    if (strcmp(*(split+1), "\"chunked\"") == 0)
                        stream->quality = GT_TWITCH_STREAM_QUALITY_SOURCE;
                    else if (strcmp(*(split+1), "\"high\"") == 0)
                        stream->quality = GT_TWITCH_STREAM_QUALITY_HIGH;
                    else if (strcmp(*(split+1), "\"medium\"") == 0)
                        stream->quality = GT_TWITCH_STREAM_QUALITY_MEDIUM;
                    else if (strcmp(*(split+1), "\"low\"") == 0)
                        stream->quality = GT_TWITCH_STREAM_QUALITY_LOW;
                    else if (strcmp(*(split+1), "\"mobile\"") == 0)
                        stream->quality = GT_TWITCH_STREAM_QUALITY_MOBILE;
                    else if (strcmp(*(split+1), "\"audio_only\"") == 0)
                        stream->quality = GT_TWITCH_STREAM_QUALITY_AUDIO_ONLY;
                }

                g_strfreev(split);
            }

            l++;

            stream->url = g_strdup(*l);

            g_strfreev(values);
        }
    }

    g_strfreev(lines);

    return ret;
}

static void
parse_channel(GtTwitch* self, JsonReader* reader, GtChannelRawData* data)
{
    json_reader_read_member(reader, "_id");
    data->id = json_reader_get_int_value(reader);
    json_reader_end_member(reader);

    json_reader_read_member(reader, "name");
    data->name = g_strdup(json_reader_get_string_value(reader));
    json_reader_end_member(reader);

    json_reader_read_member(reader, "display_name");
    data->display_name = g_strdup(json_reader_get_string_value(reader));
    json_reader_end_member(reader);

    json_reader_read_member(reader, "status");
    data->status = g_strdup(json_reader_get_string_value(reader));
    json_reader_end_member(reader);

    json_reader_read_member(reader, "video_banner");
    if (json_reader_get_null_value(reader))
        data->video_banner_url = NULL;
    else
        data->video_banner_url = g_strdup(json_reader_get_string_value(reader));
    json_reader_end_member(reader);
}

static void
parse_stream(GtTwitch* self, JsonReader* reader, GtChannelRawData* data)
{
    json_reader_read_member(reader, "game");
    if (!json_reader_get_null_value(reader))
        data->game = g_strdup(json_reader_get_string_value(reader));
    json_reader_end_member(reader);

    json_reader_read_member(reader, "viewers");
    data->viewers = json_reader_get_int_value(reader);
    json_reader_end_member(reader);

    json_reader_read_member(reader, "created_at");
    data->stream_started_time = parse_time(json_reader_get_string_value(reader));
    json_reader_end_member(reader);

    json_reader_read_member(reader, "channel");

    parse_channel(self, reader, data);

    json_reader_end_member(reader);

    json_reader_read_member(reader, "preview");

    json_reader_read_member(reader, "large");
    data->preview_url = g_strdup(json_reader_get_string_value(reader));
    json_reader_end_member(reader);

    json_reader_end_member(reader);
}

static void
parse_game(GtTwitch* self, JsonReader* reader, GtGameRawData* data, GenericTaskData* cache_update_data)
{
    gchar* id;
    gchar* filename;
    gint64 cache_timestamp;

    json_reader_read_member(reader, "_id");
    data->id = json_reader_get_int_value(reader);
    json_reader_end_member(reader);

    json_reader_read_member(reader, "name");
    data->name = g_strdup(json_reader_get_string_value(reader));
    json_reader_end_member(reader);

    id = g_strdup_printf("%ld", data->id);
    filename = g_build_filename(g_get_user_cache_dir(), "gnome-twitch", "games", id, NULL);

    if (!g_file_test(filename, G_FILE_TEST_EXISTS))
        g_info("{GtTwitch} Cache miss for game '%s'", data->name);
    else
    {
        g_info("{GtTwitch} Cache hit for game '%s'", data->name);
        data->preview = gdk_pixbuf_new_from_file(filename, NULL);
        cache_timestamp = utils_timestamp_file(filename);
    }

    json_reader_read_member(reader, "box");
    json_reader_read_member(reader, "large");

    if (!data->preview)
    {
        data->preview = gt_twitch_download_picture(self, json_reader_get_string_value(reader), 0);
        gdk_pixbuf_save(data->preview, filename, "jpeg", NULL, NULL);
    }
    else if (cache_update_data)
    {
        cache_update_data->bool_1 = TRUE;
        cache_update_data->twitch = self;
        cache_update_data->str_1 = g_strdup(json_reader_get_string_value(reader));
        cache_update_data->str_2 = g_strdup(filename);
        cache_update_data->str_3 = g_strdup(data->name);
        cache_update_data->int_1 = cache_timestamp;
    }

    json_reader_end_member(reader);
    json_reader_end_member(reader);

    g_free(id);
    g_free(filename);
}

GtTwitchStreamAccessToken*
gt_twitch_stream_access_token(GtTwitch* self, const gchar* channel)
{
    GtTwitchPrivate* priv = gt_twitch_get_instance_private(self);
    SoupMessage* msg;
    JsonParser* parser;
    JsonNode* node;
    JsonReader* reader;
    GtTwitchStreamAccessToken* ret = NULL;
    gchar uri[100];

    g_sprintf(uri, ACCESS_TOKEN_URI, channel);
    msg = soup_message_new("GET", uri);

    if (!send_message(self, msg))
    {
        g_warning("{GtTwitch} Error getting stream access token for channel '%s'", channel);
        goto finish;
    }

    ret = gt_twitch_stream_access_token_new();

    parser = json_parser_new();

    json_parser_load_from_data(parser, msg->response_body->data, msg->response_body->length, NULL);
    node = json_parser_get_root(parser);
    reader = json_reader_new(node);

    json_reader_read_member(reader, "sig");
    ret->sig = g_strdup(json_reader_get_string_value(reader));
    json_reader_end_member(reader);

    json_reader_read_member(reader, "token");
    ret->token = g_strdup(json_reader_get_string_value(reader));
    json_reader_end_member(reader);

    g_object_unref(reader);
    g_object_unref(parser);
finish:
    g_object_unref(msg);

    return ret;
}

//Not used but here if the are needed in the future
static void
stream_access_token_cb(GTask* task,
                       gpointer source,
                       gpointer task_data,
                       GCancellable* cancel)
{
    GenericTaskData* data = task_data;
    GtTwitchStreamAccessToken* ret;

    if (g_task_return_error_if_cancelled(task))
        return;

    ret = gt_twitch_stream_access_token(data->twitch, data->str_1);

    g_task_return_pointer(task, ret, (GDestroyNotify) gt_twitch_stream_access_token_free);
}

G_DEPRECATED
void
gt_twitch_stream_access_token_async(GtTwitch* self, const gchar* channel,
                                    GCancellable* cancel, GAsyncReadyCallback cb,
                                    gpointer udata)
{
    GTask* task;
    GenericTaskData* data;

    task = g_task_new(NULL, cancel, cb, udata);
    g_task_set_return_on_cancel(task, FALSE);

    data = generic_task_data_new();
    data->twitch = self;
    data->str_1 = g_strdup(channel);

    g_task_set_task_data(task, data, (GDestroyNotify) generic_task_data_free);

    g_task_run_in_thread(task, stream_access_token_cb);

    g_object_unref(task);
}

GList*
gt_twitch_all_streams(GtTwitch* self, const gchar* channel)
{
    GtTwitchPrivate* priv = gt_twitch_get_instance_private(self);
    SoupMessage* msg;
    gchar uri[500];
    GtTwitchStreamAccessToken* token;
    GList* ret = NULL;

    token = gt_twitch_stream_access_token(self, channel);

    g_return_val_if_fail(token != NULL, NULL);

    g_sprintf(uri, STREAM_PLAYLIST_URI, channel, token->token, token->sig, g_random_int_range(0, 999999));
    msg = soup_message_new("GET", uri);

    if (!send_message(self, msg))
    {
        g_warning("{GtTwitch} Error sending message to get stream uris");
        goto finish;
    }

    ret = parse_playlist(msg->response_body->data);

finish:
    g_object_unref(msg);
    gt_twitch_stream_access_token_free(token);

    return ret;
}

static void
all_streams_cb(GTask* task,
               gpointer source,
               gpointer task_data,
               GCancellable* cancel)
{
    GenericTaskData* data;
    GtTwitchStreamAccessToken* token;
    GList* ret;

    if (g_task_return_error_if_cancelled(task))
        return;

    data = task_data;

    ret = gt_twitch_all_streams(data->twitch, data->str_1);

    g_task_return_pointer(task, ret, (GDestroyNotify) gt_twitch_stream_data_list_free);
}

void
gt_twitch_all_streams_async(GtTwitch* self, const gchar* channel,
                            GCancellable* cancel, GAsyncReadyCallback cb, gpointer udata)
{
    GTask* task;
    GenericTaskData* data;

    task = g_task_new(NULL, cancel, cb, udata);
    g_task_set_return_on_cancel(task, FALSE);

    data = generic_task_data_new();
    data->twitch = self;
    data->str_1 = g_strdup(channel);

    g_task_set_task_data(task, data, (GDestroyNotify) generic_task_data_free);

    g_task_run_in_thread(task, all_streams_cb);
}

GtTwitchStreamData*
gt_twitch_stream_list_filter_quality(GList* list,
                                  GtTwitchStreamQuality qual)
{
    GtTwitchStreamData* ret = NULL;

    for (GList* l = list; l != NULL; l = l->next)
    {
        ret = (GtTwitchStreamData*) l->data;

        if (ret->quality == qual)
            break;
    }

    list = g_list_remove(list, ret);
    gt_twitch_stream_data_list_free(list);

    return ret;
}

GList*
gt_twitch_top_channels(GtTwitch* self, gint n, gint offset, gchar* game)
{
    GtTwitchPrivate* priv = gt_twitch_get_instance_private(self);
    SoupMessage* msg;
    gchar* uri;
    JsonParser* parser;
    JsonNode* node;
    JsonReader* reader;
    JsonArray* channels;
    GList* ret = NULL;

    uri = g_strdup_printf(TOP_CHANNELS_URI, n, offset, game);
    msg = soup_message_new("GET", uri);

    if (!send_message(self, msg))
    {
	g_warning("{GtTwitch} Error sending message to get top channels");
        goto finish;
    }

    parser = json_parser_new();
    json_parser_load_from_data(parser, msg->response_body->data, msg->response_body->length, NULL);
    node = json_parser_get_root(parser);
    reader = json_reader_new(node);

    json_reader_read_member(reader, "streams");

    for (gint i = 0; i < json_reader_count_elements(reader); i++)
    {
        GtChannel* channel;
        GtChannelRawData* data = g_malloc0(sizeof(GtChannelRawData));;

        data->viewers = -1;
        data->online = TRUE;

        json_reader_read_element(reader, i);

        parse_stream(self, reader, data);
        channel = gt_channel_new(data->name, data->id);
        g_object_force_floating(G_OBJECT(channel));
        gt_channel_update_from_raw_data(channel, data);

        json_reader_end_element(reader);

        ret = g_list_append(ret, channel);

        gt_twitch_channel_raw_data_free(data);
    }
    json_reader_end_member(reader);

    g_object_unref(parser);
    g_object_unref(reader);
finish:
    g_object_unref(msg);
    g_free(uri);

    return ret;
}

GList*
gt_twitch_top_games(GtTwitch* self,
                    gint n, gint offset)
{
    GtTwitchPrivate* priv = gt_twitch_get_instance_private(self);
    SoupMessage* msg;
    gchar* uri;
    JsonParser* parser;
    JsonNode* node;
    JsonReader* reader;
    JsonArray* channels;
    GList* ret = NULL;

    uri = g_strdup_printf(TOP_GAMES_URI, n, offset);
    msg = soup_message_new("GET", uri);

    if (!send_message(self, msg))
    {
        g_warning("{GtTwitch} Error sending message to get top channels");
        goto finish;
    }

    parser = json_parser_new();
    json_parser_load_from_data(parser, msg->response_body->data, msg->response_body->length, NULL);
    node = json_parser_get_root(parser);
    reader = json_reader_new(node);

    json_reader_read_member(reader, "top");

    for (gint i = 0; i < json_reader_count_elements(reader); i++)
    {
        GtGame* game;
        GtGameRawData* data = g_malloc0(sizeof(GtGameRawData));
        GenericTaskData* cache_update_data = generic_task_data_new();

        json_reader_read_element(reader, i);

        json_reader_read_member(reader, "game");

        parse_game(self, reader, data, cache_update_data);
        game = gt_game_new(data->name, data->id);
        g_object_force_floating(G_OBJECT(game));
        gt_game_update_from_raw_data(game, data);

        if (cache_update_data->bool_1)
        {
            cache_update_data->gobject_ptr_1 = g_object_ref(game);
            g_thread_pool_push(cache_update_pool, cache_update_data, NULL);
        }
        else
            generic_task_data_free(cache_update_data);

        json_reader_end_member(reader);

        json_reader_read_member(reader, "viewers");
        g_object_set(game, "viewers", json_reader_get_int_value(reader), NULL);
        json_reader_end_member(reader);

        json_reader_read_member(reader, "channels");
        g_object_set(game, "channels", json_reader_get_int_value(reader), NULL);
        json_reader_end_member(reader);

        json_reader_end_element(reader);

        ret = g_list_append(ret, game);

        gt_twitch_game_raw_data_free(data);
    }

    json_reader_end_member(reader);

    g_object_unref(parser);
    g_object_unref(reader);
finish:
    g_object_unref(msg);
    g_free(uri);

    return ret;
}

static void
top_channels_async_cb(GTask* task,
                      gpointer source,
                      gpointer task_data,
                      GCancellable* cancel)
{
    GenericTaskData* data = task_data;
    GList* ret;

    if (g_task_return_error_if_cancelled(task))
        return;

    ret = gt_twitch_top_channels(data->twitch, data->int_1, data->int_2, data->str_1);

    if (!ret)
    {
        g_task_return_new_error(task, GT_TWITCH_ERROR_TOP_CHANNELS, GT_TWITCH_TOP_CHANNELS_ERROR_CODE,
                                "Error getting top channels");
    }
    else
        g_task_return_pointer(task, ret, (GDestroyNotify) gt_channel_free_list);
}

void
gt_twitch_top_channels_async(GtTwitch* self, gint n, gint offset, gchar* game,
                             GCancellable* cancel,
                             GAsyncReadyCallback cb,
                             gpointer udata)
{
    GTask* task = NULL;
    GenericTaskData* data = NULL;

    task = g_task_new(NULL, cancel, cb, udata);
    g_task_set_return_on_cancel(task, FALSE);

    data = generic_task_data_new();
    data->twitch = self;
    data->int_1 = n;
    data->int_2 = offset;
    data->str_1 = g_strdup(game);

    g_task_set_task_data(task, data, (GDestroyNotify) generic_task_data_free);

    g_task_run_in_thread(task, top_channels_async_cb);

    g_object_unref(task);
}

static void
top_games_async_cb(GTask* task,
                   gpointer source,
                   gpointer task_data,
                   GCancellable* cancel)
{
    GenericTaskData* data = task_data;
    GList* ret;

    if (g_task_return_error_if_cancelled(task))
        return;

    ret = gt_twitch_top_games(data->twitch, data->int_1, data->int_2);

    g_task_return_pointer(task, ret, (GDestroyNotify) gt_game_free_list);
}

void
gt_twitch_top_games_async(GtTwitch* self, gint n, gint offset,
                             GCancellable* cancel,
                             GAsyncReadyCallback cb,
                             gpointer udata)
{
    GTask* task = NULL;
    GenericTaskData* data = NULL;

    task = g_task_new(NULL, cancel, cb, udata);
    g_task_set_return_on_cancel(task, FALSE);

    data = generic_task_data_new();
    data->twitch = self;
    data->int_1 = n;
    data->int_2 = offset;

    g_task_set_task_data(task, data, (GDestroyNotify) generic_task_data_free);

    g_task_run_in_thread(task, top_games_async_cb);

    g_object_unref(task);
}

GList*
gt_twitch_search_channels(GtTwitch* self, const gchar* query, gint n, gint offset)
{
    GtTwitchPrivate* priv = gt_twitch_get_instance_private(self);
    SoupMessage* msg;
    gchar* uri;
    JsonParser* parser;
    JsonNode* node;
    JsonReader* reader;
    JsonArray* channels;
    GList* ret = NULL;

    uri = g_strdup_printf(SEARCH_CHANNELS_URI, query, n, offset);
    msg = soup_message_new("GET", uri);

    if (!send_message(self, msg))
    {
        g_warning("{GtTwitch} Error sending message to search channels");
        goto finish;
    }

    parser = json_parser_new();
    json_parser_load_from_data(parser, msg->response_body->data, msg->response_body->length, NULL);
    node = json_parser_get_root(parser);
    reader = json_reader_new(node);

    json_reader_read_member(reader, "streams");
    for (gint i = 0; i < json_reader_count_elements(reader); i++)
    {
        GtChannel* channel;
        GtChannelRawData* data = g_malloc0(sizeof(GtChannelRawData));

        data->online = TRUE;
        data->viewers = -1;

        json_reader_read_element(reader, i);

        parse_stream(self, reader, data);
        channel = gt_channel_new(data->name, data->id);
        g_object_force_floating(G_OBJECT(channel));
        gt_channel_update_from_raw_data(channel, data);

        json_reader_end_element(reader);

        ret = g_list_append(ret, channel);

        gt_twitch_channel_raw_data_free(data);
    }
    json_reader_end_member(reader);

    g_object_unref(parser);
    g_object_unref(reader);
finish:
    g_object_unref(msg);
    g_free(uri);

    return ret;
}

GList*
gt_twitch_search_games(GtTwitch* self, const gchar* query, gint n, gint offset)
{
    GtTwitchPrivate* priv = gt_twitch_get_instance_private(self);
    SoupMessage* msg;
    gchar* uri;
    JsonParser* parser;
    JsonNode* node;
    JsonReader* reader;
    JsonArray* channels;
    GList* ret = NULL;

    uri = g_strdup_printf(SEARCH_GAMES_URI, query);
    msg = soup_message_new("GET", uri);

    if (!send_message(self, msg))
    {
        g_warning("{GtTwitch} Error sending message to search games");
        goto finish;
    }

    parser = json_parser_new();
    json_parser_load_from_data(parser, msg->response_body->data, msg->response_body->length, NULL);
    node = json_parser_get_root(parser);
    reader = json_reader_new(node);

    json_reader_read_member(reader, "games");
    for (gint i = 0; i < json_reader_count_elements(reader); i++)
    {
        GtGame* game;
        GtGameRawData* data = g_malloc0(sizeof(GtGameRawData));
        GenericTaskData* cache_update_data = generic_task_data_new();

        json_reader_read_element(reader, i);

        parse_game(self, reader, data, cache_update_data);
        game = gt_game_new(data->name, data->id);
        g_object_force_floating(G_OBJECT(game));
        gt_game_update_from_raw_data(game, data);

        if (cache_update_data->bool_1)
        {
            cache_update_data->gobject_ptr_1 = g_object_ref(game);
            g_thread_pool_push(cache_update_pool, cache_update_data, NULL);
        }
        else
            generic_task_data_free(cache_update_data);

        json_reader_end_element(reader);

        ret = g_list_append(ret, game);

        gt_twitch_game_raw_data_free(data);
    }
    json_reader_end_member(reader);

    g_object_unref(parser);
    g_object_unref(reader);
finish:
    g_object_unref(msg);
    g_free(uri);

    return ret;
}

static void
search_channels_async_cb(GTask* task,
                         gpointer source,
                         gpointer task_data,
                         GCancellable* cancel)
{
    GenericTaskData* data = task_data;
    GList* ret;

    if (g_task_return_error_if_cancelled(task))
        return;

    ret = gt_twitch_search_channels(data->twitch, data->str_1, data->int_1, data->int_2);

    if (!ret)
    {
        g_task_return_new_error(task, GT_TWITCH_ERROR_SEARCH_CHANNELS, GT_TWITCH_SEARCH_CHANNELS_ERROR_CODE,
                                "Error searching channels");
    }
    else
        g_task_return_pointer(task, ret, (GDestroyNotify) gt_channel_free_list);
}

void
gt_twitch_search_channels_async(GtTwitch* self, const gchar* query,
                                gint n, gint offset,
                                GCancellable* cancel,
                                GAsyncReadyCallback cb,
                                gpointer udata)
{
    GTask* task = NULL;
    GenericTaskData* data = NULL;

    task = g_task_new(NULL, cancel, cb, udata);
    g_task_set_return_on_cancel(task, FALSE);

    data = generic_task_data_new();
    data->twitch = self;
    data->int_1 = n;
    data->int_2 = offset;
    data->str_1 = g_strdup(query);

    g_task_set_task_data(task, data, (GDestroyNotify) generic_task_data_free);

    g_task_run_in_thread(task, search_channels_async_cb);

    g_object_unref(task);
}

static void
search_games_async_cb(GTask* task,
                      gpointer source,
                      gpointer task_data,
                      GCancellable* cancel)
{
    GenericTaskData* data = task_data;
    GList* ret;

    if (g_task_return_error_if_cancelled(task))
        return;

    ret = gt_twitch_search_games(data->twitch, data->str_1, data->int_1, data->int_2);

    g_task_return_pointer(task, ret, (GDestroyNotify) gt_game_free_list);
}

void
gt_twitch_search_games_async(GtTwitch* self, const gchar* query,
                             gint n, gint offset,
                             GCancellable* cancel,
                             GAsyncReadyCallback cb,
                             gpointer udata)
{
    GTask* task = NULL;
    GenericTaskData* data = NULL;

    task = g_task_new(NULL, cancel, cb, udata);
    g_task_set_return_on_cancel(task, FALSE);

    data = generic_task_data_new();
    data->twitch = self;
    data->int_1 = n;
    data->int_2 = offset;
    data->str_1 = g_strdup(query);

    g_task_set_task_data(task, data, (GDestroyNotify) generic_task_data_free);

    g_task_run_in_thread(task, search_games_async_cb);

    g_object_unref(task);
}

void
gt_twitch_stream_data_free(GtTwitchStreamData* data)
{
    g_free(data->url);
    g_free(data);
}

void
gt_twitch_stream_data_list_free(GList* list)
{
    g_list_free_full(list, (GDestroyNotify) gt_twitch_stream_data_free);
}

GtChannelRawData*
gt_twitch_channel_raw_data(GtTwitch* self, const gchar* name)
{
    GtTwitchPrivate* priv = gt_twitch_get_instance_private(self);
    SoupMessage* msg;
    gchar* uri;
    JsonParser* parser;
    JsonNode* node;
    JsonReader* reader;
    GtChannelRawData* ret = NULL;

    uri = g_strdup_printf(CHANNELS_URI, name);
    msg = soup_message_new("GET", uri);

    if (!send_message(self, msg))
    {
        g_warning("{GtTwitch} Error sending message to get raw channel data");
        goto finish;
    }

    parser = json_parser_new();
    json_parser_load_from_data(parser, msg->response_body->data, msg->response_body->length, NULL);
    node = json_parser_get_root(parser);
    reader = json_reader_new(node);

    ret = g_malloc0_n(1, sizeof(GtChannelRawData));
    parse_channel(self, reader, ret);

    g_object_unref(parser);
    g_object_unref(reader);
finish:
    g_object_unref(msg);
    g_free(uri);

    return ret;
}

GtChannelRawData*
gt_twitch_channel_with_stream_raw_data(GtTwitch* self, const gchar* name)
{
    GtTwitchPrivate* priv = gt_twitch_get_instance_private(self);
    SoupMessage* msg;
    gchar* uri;
    JsonParser* parser;
    JsonNode* node;
    JsonReader* reader;
    GtChannelRawData* ret = NULL;

    uri = g_strdup_printf(STREAMS_URI, name);
    msg = soup_message_new("GET", uri);

    if (!send_message(self, msg))
        goto finish;

    parser = json_parser_new();
    json_parser_load_from_data(parser, msg->response_body->data, msg->response_body->length, NULL);
    node = json_parser_get_root(parser);
    reader = json_reader_new(node);

    json_reader_read_member(reader, "stream");

    if (json_reader_get_null_value(reader))
    {
        ret = gt_twitch_channel_raw_data(self, name);

        if (ret) ret->online = FALSE;
    }
    else
    {
        ret = g_malloc0_n(1, sizeof(GtChannelRawData));
        parse_stream(self, reader, ret);
        ret->online = TRUE;
    }

    json_reader_end_member(reader);

    g_object_unref(parser);
    g_object_unref(reader);
finish:
    g_object_unref(msg);
    g_free(uri);

    return ret;
}

static void
channel_raw_data_cb(GTask* task,
                    gpointer source,
                    gpointer task_data,
                    GCancellable* cancel)
{
    GtChannelRawData* ret;
    GenericTaskData* data = task_data;

    if (g_task_return_error_if_cancelled(task))
        return;

    ret = gt_twitch_channel_raw_data(data->twitch, data->str_1);

    g_task_return_pointer(task, ret, (GDestroyNotify) gt_twitch_channel_raw_data_free);
}

// This was a bit unecessary, but at least it's here if it's ever needed
G_DEPRECATED
void
gt_twitch_channel_raw_data_async(GtTwitch* self, const gchar* name,
                          GCancellable* cancel,
                          GAsyncReadyCallback cb,
                          gpointer udata)
{
    GTask* task = NULL;
    GenericTaskData* data;

    task = g_task_new(NULL, cancel, cb, udata);
    g_task_set_return_on_cancel(task, FALSE);

    data = g_new0(GenericTaskData, 1);
    data->twitch = self;
    data->str_1 = g_strdup(name);
    g_task_set_task_data(task, data, (GDestroyNotify) generic_task_data_free);

    g_task_run_in_thread(task, channel_raw_data_cb);

    g_object_unref(task);
}

void
gt_twitch_channel_raw_data_free(GtChannelRawData* data)
{
    g_free(data->game);
    g_free(data->status);
    g_free(data->display_name);
    g_free(data->preview_url);
    g_free(data->video_banner_url);
    if (data->stream_started_time)
        g_date_time_unref(data->stream_started_time);

    g_free(data);
}

GtGameRawData*
gt_game_raw_data(GtTwitch* self, const gchar* name)
{
    //TODO not possible with current API

    return NULL;
}

void
gt_twitch_game_raw_data_free(GtGameRawData* data)
{
    g_free(data->name);
    g_object_unref(data->preview);
    g_free(data);
}

GdkPixbuf*
gt_twitch_download_picture(GtTwitch* self, const gchar* url, gint64 timestamp)
{
    GtTwitchPrivate* priv = gt_twitch_get_instance_private(self);

    g_info("{GtTwitch} Downloading picture from url '%s'", url);

    if (!url || strlen(url) < 1)
        return NULL;

    if (timestamp)
        return utils_download_picture_if_newer(priv->soup, url, timestamp);
    else
        return utils_download_picture(priv->soup, url);
}

static void
download_picture_async_cb(GTask* task,
                          gpointer source,
                          gpointer task_data,
                          GCancellable* cancel)
{
    GenericTaskData* data = (GenericTaskData*) task_data;
    GdkPixbuf* ret = NULL;

    if (g_task_return_error_if_cancelled(task))
        return;

    ret = gt_twitch_download_picture(data->twitch, data->str_1, data->int_1);

    g_task_return_pointer(task, ret, (GDestroyNotify) g_object_unref);
}

void
gt_twitch_download_picture_async(GtTwitch* self,
                                 const gchar* url,
                                 gint64 timestamp,
                                 GCancellable* cancel,
                                 GAsyncReadyCallback cb,
                                 gpointer udata)
{
    GTask* task = NULL;
    GenericTaskData* data = NULL;

    task = g_task_new(NULL, cancel, cb, udata);
    g_task_set_return_on_cancel(task, FALSE);

    data = generic_task_data_new();
    data->twitch = self;
    data->str_1 = g_strdup(url);
    data->int_1 = timestamp;

    g_task_set_task_data(task, data, (GDestroyNotify) generic_task_data_free);

    g_task_run_in_thread(task, download_picture_async_cb);

    g_object_unref(task);
}

GdkPixbuf*
gt_twitch_download_emote(GtTwitch* self, gint id)
{
    GtTwitchPrivate* priv = gt_twitch_get_instance_private(self);
    gchar url[128];

    g_sprintf(url, TWITCH_EMOTE_URI, id, 1);

    g_info("{GtTwitch} Downloading emote from url '%s'", url);

    return utils_download_picture(priv->soup, url);
}

static GtChatBadges*
gt_chat_badges_new()
{
    return g_new0(GtChatBadges, 1);
}

void
gt_chat_badges_free(GtChatBadges* badges)
{
    g_assert_nonnull(badges);

    g_clear_object(&badges->global_mod);
    g_clear_object(&badges->admin);
    g_clear_object(&badges->broadcaster);
    g_clear_object(&badges->mod);
    g_clear_object(&badges->staff);
    g_clear_object(&badges->turbo);
    if (badges->subscriber)
        g_clear_object(&badges->subscriber);
    g_free(badges);
}

GtChatBadges*
gt_chat_badges(GtTwitch* self, const gchar* chan)
{
    GtTwitchPrivate* priv = gt_twitch_get_instance_private(self);
    SoupMessage* msg;
    gchar uri[100];
    JsonParser* parser;
    JsonNode* node;
    JsonReader* reader;
    GtChatBadges* ret = NULL;

    g_info("{GtTwitch} Getting chat badges for channel '%s'", chan);

    g_sprintf(uri, CHAT_BADGES_URI, chan);

    msg = soup_message_new("GET", uri);

    if (!send_message(self, msg))
    {
        g_warning("{GtTwitch} Error getting chat badges for channel '%s'", chan);
        goto finish;
    }

    parser = json_parser_new();
    json_parser_load_from_data(parser, msg->response_body->data, msg->response_body->length, NULL); //TODO: Error handling
    node = json_parser_get_root(parser);
    reader = json_reader_new(node);

    ret = gt_chat_badges_new();

    json_reader_read_member(reader, "global_mod");
    json_reader_read_member(reader, "image");
    ret->global_mod = utils_download_picture(priv->soup, json_reader_get_string_value(reader));
    json_reader_end_member(reader);
    json_reader_end_member(reader);

    json_reader_read_member(reader, "admin");
    json_reader_read_member(reader, "image");
    ret->admin = utils_download_picture(priv->soup, json_reader_get_string_value(reader));
    json_reader_end_member(reader);
    json_reader_end_member(reader);

    json_reader_read_member(reader, "broadcaster");
    json_reader_read_member(reader, "image");
    ret->broadcaster = utils_download_picture(priv->soup, json_reader_get_string_value(reader));
    json_reader_end_member(reader);
    json_reader_end_member(reader);

    json_reader_read_member(reader, "mod");
    json_reader_read_member(reader, "image");
    ret->mod = utils_download_picture(priv->soup, json_reader_get_string_value(reader));
    json_reader_end_member(reader);
    json_reader_end_member(reader);

    json_reader_read_member(reader, "staff");
    json_reader_read_member(reader, "image");
    ret->staff = utils_download_picture(priv->soup, json_reader_get_string_value(reader));
    json_reader_end_member(reader);
    json_reader_end_member(reader);

    json_reader_read_member(reader, "turbo");
    json_reader_read_member(reader, "image");
    ret->turbo = utils_download_picture(priv->soup, json_reader_get_string_value(reader));
    json_reader_end_member(reader);
    json_reader_end_member(reader);

    json_reader_read_member(reader, "subscriber");
    if (!json_reader_get_null_value(reader))
    {
        json_reader_read_member(reader, "image");
        ret->subscriber = utils_download_picture(priv->soup, json_reader_get_string_value(reader));
        json_reader_end_member(reader);
    }
    json_reader_end_member(reader);

    g_object_unref(parser);
    g_object_unref(reader);

finish:
    g_object_unref(msg);

    return ret;
}

static void
chat_badges_async_cb(GTask* task,
                     gpointer source,
                     gpointer task_data,
                     GCancellable* cancel)
{
    GenericTaskData* data;
    GtChatBadges* ret;

    if (g_task_return_error_if_cancelled(task))
        return;

    data = task_data;

    ret = gt_chat_badges(data->twitch, data->str_1);

    g_task_return_pointer(task, ret, (GDestroyNotify) gt_chat_badges_free);
}

void
gt_chat_badges_async(GtTwitch* self, const gchar* channel,
                            GCancellable* cancel, GAsyncReadyCallback cb, gpointer udata)
{
    GTask* task;
    GenericTaskData* data;

    task = g_task_new(NULL, cancel, cb, udata);
    g_task_set_return_on_cancel(task, FALSE);

    data = generic_task_data_new();
    data->twitch = self;
    data->str_1 = g_strdup(channel);

    g_task_set_task_data(task, data, (GDestroyNotify) generic_task_data_free);

    g_task_run_in_thread(task, chat_badges_async_cb);

    g_object_unref(task);
}

static GtTwitchChannelInfoPanel*
gt_twitch_channel_info_panel_new()
{
    return g_new0(GtTwitchChannelInfoPanel, 1);
}

void
gt_twitch_channel_info_panel_free(GtTwitchChannelInfoPanel* panel)
{
    g_free(panel->html_description);
    g_free(panel->markdown_description);
    g_clear_object(&panel->image);
    g_free(panel->link);
    g_free(panel);
}

void
gt_twitch_channel_info_panel_list_free(GList* list)
{
    g_list_free_full(list, (GDestroyNotify) gt_twitch_channel_info_panel_free);
}

GList*
gt_twitch_channel_info(GtTwitch* self, const gchar* chan)
{
    GtTwitchPrivate* priv = gt_twitch_get_instance_private(self);
    SoupMessage* msg;
    gchar uri[100];
    JsonParser* parser;
    JsonNode* node;
    JsonReader* reader;
    GList* ret = NULL;

    g_info("{GtTwitch} Getting channel info for '%s'", chan);

    g_sprintf(uri, CHANNEL_INFO_URI, chan);

    msg = soup_message_new("GET", uri);

    if (!send_message(self, msg))
    {
        g_warning("{GtTwitch} Error getting chat badges for channel '%s'", chan);
        goto finish;
    }

    parser = json_parser_new();
    json_parser_load_from_data(parser, msg->response_body->data, msg->response_body->length, NULL); //TODO: Error handling
    node = json_parser_get_root(parser);
    reader = json_reader_new(node);

    for (gint i = 0; i < json_reader_count_elements(reader); i++)
    {
        GtTwitchChannelInfoPanel* panel = gt_twitch_channel_info_panel_new();
        const gchar* type = NULL;

        json_reader_read_element(reader, i);

        json_reader_read_member(reader, "display_order");
        panel->order = json_reader_get_int_value(reader) - 1;
        json_reader_end_member(reader);

        json_reader_read_member(reader, "kind");
        type = json_reader_get_string_value(reader);
        if (g_strcmp0(type, "default") == 0)
        {
            panel->type = GT_TWITCH_CHANNEL_INFO_PANEL_TYPE_DEFAULT;
        }
        else
        {
            //TODO: Eventually handle other types of panels
            gt_twitch_channel_info_panel_free(panel);
            json_reader_end_member(reader);
            json_reader_end_element(reader);
            continue;
        }
        json_reader_end_member(reader);

        json_reader_read_member(reader, "html_description");
        if (!json_reader_get_null_value(reader))
            panel->html_description = g_strdup(json_reader_get_string_value(reader));
        json_reader_end_member(reader);

        json_reader_read_member(reader, "data");

        json_reader_read_member(reader, "link");
        panel->link = g_strdup(json_reader_get_string_value(reader));
        json_reader_end_member(reader);

        json_reader_read_member(reader, "image");
        panel->image = gt_twitch_download_picture(self, json_reader_get_string_value(reader), 0);
        json_reader_end_member(reader);

        if (json_reader_read_member(reader, "description"))
            panel->markdown_description = g_strdup(json_reader_get_string_value(reader));
        json_reader_end_member(reader);

        if (json_reader_read_member(reader, "title"))
            panel->title = g_strdup(json_reader_get_string_value(reader));
        json_reader_end_member(reader);

        json_reader_end_member(reader);

        json_reader_end_element(reader);

        ret = g_list_append(ret, panel);
    }

    g_object_unref(parser);
    g_object_unref(reader);

finish:
    g_object_unref(msg);

    return ret;
}


static void
channel_info_async_cb(GTask* task,
                      gpointer source,
                      gpointer task_data,
                      GCancellable* cancel)
{
    GenericTaskData* data;
    GList* ret;

    if (g_task_return_error_if_cancelled(task))
        return;

    data = task_data;

    ret = gt_twitch_channel_info(data->twitch, data->str_1);

    g_task_return_pointer(task, ret, (GDestroyNotify) gt_twitch_channel_info_panel_list_free);
}

void
gt_twitch_channel_info_async(GtTwitch* self, const gchar* chan,
                             GCancellable* cancel, GAsyncReadyCallback cb, gpointer udata)
{
    GTask* task;
    GenericTaskData* data;

    task = g_task_new(NULL, cancel, cb, udata);
    g_task_set_return_on_cancel(task, FALSE);

    data = generic_task_data_new();
    data->twitch = self;
    data->str_1 = g_strdup(chan);

    g_task_set_task_data(task, data, (GDestroyNotify) generic_task_data_free);

    g_task_run_in_thread(task, channel_info_async_cb);

    g_object_unref(task);
}

GList*
gt_twitch_chat_servers(GtTwitch* self,
                       const gchar* chan)
{
    GtTwitchPrivate* priv = gt_twitch_get_instance_private(self);
    SoupMessage* msg;
    gchar* uri;
    JsonParser* parser;
    JsonNode* node;
    JsonReader* reader;
    GList* ret = NULL;

    uri = g_strdup_printf(CHAT_SERVERS_URI, chan);
    msg = soup_message_new("GET", uri);

    if (!send_message(self, msg))
        goto finish;

    parser = json_parser_new();
    json_parser_load_from_data(parser, msg->response_body->data, msg->response_body->length, NULL);
    node = json_parser_get_root(parser);
    reader = json_reader_new(node);

    json_reader_read_member(reader, "chat_servers");

    for (int i = 0; i < json_reader_count_elements(reader); i++)
    {
        json_reader_read_element(reader, i);
        ret = g_list_append(ret, g_strdup(json_reader_get_string_value(reader)));
        json_reader_end_element(reader);
    }

    json_reader_end_member(reader);

    g_object_unref(parser);
    g_object_unref(reader);

finish:
    g_object_unref(msg);

    return ret;
}
