/*
 * Copyright 2018-present MongoDB, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include "mongoc.h"
#include "mongoc-cursor-private.h"
#include "mongoc-client-private.h"

typedef struct _data_find_cmd_t {
   mongoc_cursor_response_t response;
} data_find_cmd_t;


static void
_destroy (mongoc_cursor_context_t *ctx)
{
   data_find_cmd_t *data = (data_find_cmd_t *) ctx->data;
   bson_destroy (&data->response.reply);
   bson_free (data);
}


static void
_prime (mongoc_cursor_t *cursor)
{
   data_find_cmd_t *data = (data_find_cmd_t *) cursor->ctx.data;
   bson_t find_cmd;
   bson_init (&find_cmd);
   /* TODO: The legacy cursor sets the operation ID in _mongoc_cursor_op_query,
    * the cursorid cursor set it during priming. Which is better? */
   cursor->operation_id = ++cursor->client->cluster.operation_id;
   /* construct { find: "<collection>", filter: {<filter>} } */
   _mongoc_cursor_prepare_find_command (cursor, &find_cmd);
   _mongoc_cursor_response_refresh (
      cursor, &find_cmd, &cursor->opts, &data->response);
   bson_destroy (&find_cmd);
}


static void
_pop_from_batch (mongoc_cursor_t *cursor, const bson_t **out)
{
   data_find_cmd_t *data = (data_find_cmd_t *) cursor->ctx.data;
   _mongoc_cursor_response_read (cursor, &data->response, out);
}


static void
_get_next_batch (mongoc_cursor_t *cursor)
{
   data_find_cmd_t *ctx = (data_find_cmd_t *) cursor->ctx.data;
   bson_t getmore_cmd;
   _mongoc_cursor_prepare_getmore_command (cursor, &getmore_cmd);
   _mongoc_cursor_response_refresh (
      cursor, &getmore_cmd, NULL /* opts */, &ctx->response);
   bson_destroy (&getmore_cmd);
}


static void
_clone (mongoc_cursor_context_t *dst, const mongoc_cursor_context_t *src)
{
   data_find_cmd_t *data = bson_malloc0 (sizeof (*data));
   bson_init (&data->response.reply);
   dst->data = data;
}


/* transition a find cursor to use the find command. */
void
_mongoc_cursor_ctx_find_cmd_init (mongoc_cursor_t *cursor)
{
   data_find_cmd_t *data = bson_malloc0 (sizeof (*data));
   bson_init (&data->response.reply);
   cursor->ctx.prime = _prime;
   cursor->ctx.pop_from_batch = _pop_from_batch;
   cursor->ctx.get_next_batch = _get_next_batch;
   cursor->ctx.destroy = _destroy;
   cursor->ctx.clone = _clone;
   cursor->ctx.data = (void *) data;
}