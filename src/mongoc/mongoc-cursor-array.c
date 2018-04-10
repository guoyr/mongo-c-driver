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

typedef struct _data_array_t {
   bson_t array;
   bson_iter_t iter;
   bson_t bson; /* current document */
   const char *field_name;
} data_array_t;


static void
_destroy (mongoc_cursor_ctx_t *ctx)
{
   data_array_t *data = (data_array_t *) ctx->data;
   bson_destroy (&data->array);
   bson_free (data);
}


static void
_prime (mongoc_cursor_t *cursor)
{
   bson_iter_t iter;
   data_array_t *data = (data_array_t *) cursor->ctx.data;

   bson_destroy (&data->array);
   if (_mongoc_cursor_run_command (
          cursor, &cursor->filter, &cursor->opts, &data->array) &&
       bson_iter_init_find (&iter, &data->array, data->field_name) &&
       BSON_ITER_HOLDS_ARRAY (&iter) &&
       bson_iter_recurse (&iter, &data->iter)) {
      cursor->state = IN_BATCH;
      return;
   }
   cursor->state = DONE;
}


static void
_pop_from_batch (mongoc_cursor_t *cursor, const bson_t **out)
{
   uint32_t document_len;
   const uint8_t *document;
   data_array_t *data = (data_array_t *) cursor->ctx.data;
   if (bson_iter_next (&data->iter)) {
      bson_iter_document (&data->iter, &document_len, &document);
      bson_init_static (&data->bson, document, document_len);
      *out = &data->bson;
   } else {
      cursor->state = DONE;
   }
}

static void
_clone (mongoc_cursor_ctx_t *dst, const mongoc_cursor_ctx_t *src)
{
   data_array_t *data = bson_malloc0 (sizeof (*data));
   bson_init (&data->array);
   dst->data = data;
}

mongoc_cursor_t *
_mongoc_cursor_array_new (mongoc_client_t *client,
                          const char *db_and_coll,
                          const bson_t *cmd,
                          const bson_t *opts,
                          const char *field_name)
{
   mongoc_cursor_t *cursor =
      _mongoc_cursor_new_with_opts (client, db_and_coll, cmd, opts, NULL, NULL);
   data_array_t *data = bson_malloc0 (sizeof (*data));
   bson_init (&data->array);
   data->field_name = field_name;
   cursor->ctx.prime = _prime;
   cursor->ctx.pop_from_batch = _pop_from_batch;
   cursor->ctx.destroy = _destroy;
   cursor->ctx.clone = _clone;
   cursor->ctx.data = (void *) data;
   return cursor;
}