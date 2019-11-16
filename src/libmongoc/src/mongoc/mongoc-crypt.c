/*
 * Copyright 2019-present MongoDB, Inc.
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

#define MONGOC_LOG_DOMAIN "client-side-encryption"

#include "mongoc/mongoc-crypt-private.h"

#ifdef MONGOC_ENABLE_CLIENT_SIDE_ENCRYPTION

#include <mongocrypt/mongocrypt.h>

#include "mongoc/mongoc-client-private.h"
#include "mongoc/mongoc-collection-private.h"
#include "mongoc/mongoc-host-list-private.h"
#include "mongoc/mongoc-stream-private.h"

struct __mongoc_crypt_t {
   mongocrypt_t *handle;
};

static void
_log_callback (mongocrypt_log_level_t mongocrypt_log_level,
               const char *message,
               uint32_t message_len,
               void *ctx)
{
   mongoc_log_level_t log_level = MONGOC_LOG_LEVEL_ERROR;

   switch (mongocrypt_log_level) {
   case MONGOCRYPT_LOG_LEVEL_FATAL:
      log_level = MONGOC_LOG_LEVEL_CRITICAL;
      break;
   case MONGOCRYPT_LOG_LEVEL_ERROR:
      log_level = MONGOC_LOG_LEVEL_ERROR;
      break;
   case MONGOCRYPT_LOG_LEVEL_WARNING:
      log_level = MONGOC_LOG_LEVEL_WARNING;
      break;
   case MONGOCRYPT_LOG_LEVEL_INFO:
      log_level = MONGOC_LOG_LEVEL_INFO;
      break;
   case MONGOCRYPT_LOG_LEVEL_TRACE:
      log_level = MONGOC_LOG_LEVEL_TRACE;
      break;
   }

   mongoc_log (log_level, MONGOC_LOG_DOMAIN, "%s", message, NULL);
}

static void
_prefix_mongocryptd_error (bson_error_t *error)
{
   char buf[sizeof (error->message)];

   bson_snprintf (buf, sizeof (buf), "mongocryptd error: %s:", error->message);
   memcpy (error->message, buf, sizeof (buf));
}

static void
_prefix_keyvault_error (bson_error_t *error)
{
   char buf[sizeof (error->message)];

   bson_snprintf (buf, sizeof (buf), "key vault error: %s:", error->message);
   memcpy (error->message, buf, sizeof (buf));
}

static void
_status_to_error (mongocrypt_status_t *status, bson_error_t *error)
{
   bson_set_error (error,
                   MONGOC_ERROR_CLIENT_SIDE_ENCRYPTION,
                   mongocrypt_status_code (status),
                   "%s",
                   mongocrypt_status_message (status, NULL));
}

/* Checks for an error on mongocrypt context.
 * If error_expected, then we expect mongocrypt_ctx_status to report a failure
 * status (due to a previous failed function call). If it did not, return a
 * generic error.
 * Returns true if ok, and does not modify @error.
 * Returns false if error, and sets @error.
 */
bool
_ctx_check_error (mongocrypt_ctx_t *ctx,
                  bson_error_t *error,
                  bool error_expected)
{
   mongocrypt_status_t *status;

   status = mongocrypt_status_new ();
   if (!mongocrypt_ctx_status (ctx, status)) {
      _status_to_error (status, error);
      mongocrypt_status_destroy (status);
      return false;
   } else if (error_expected) {
      bson_set_error (error,
                      MONGOC_ERROR_CLIENT,
                      MONGOC_ERROR_CLIENT_INVALID_ENCRYPTION_STATE,
                      "generic error from libmongocrypt operation");
      mongocrypt_status_destroy (status);
      return false;
   }
   mongocrypt_status_destroy (status);
   return true;
}

bool
_kms_ctx_check_error (mongocrypt_kms_ctx_t *kms_ctx,
                      bson_error_t *error,
                      bool error_expected)
{
   mongocrypt_status_t *status;

   status = mongocrypt_status_new ();
   if (!mongocrypt_kms_ctx_status (kms_ctx, status)) {
      _status_to_error (status, error);
      mongocrypt_status_destroy (status);
      return false;
   } else if (error_expected) {
      bson_set_error (error,
                      MONGOC_ERROR_CLIENT,
                      MONGOC_ERROR_CLIENT_INVALID_ENCRYPTION_STATE,
                      "generic error from libmongocrypt KMS operation");
      mongocrypt_status_destroy (status);
      return false;
   }
   mongocrypt_status_destroy (status);
   return true;
}

bool
_crypt_check_error (mongocrypt_t *crypt,
                    bson_error_t *error,
                    bool error_expected)
{
   mongocrypt_status_t *status;

   status = mongocrypt_status_new ();
   if (!mongocrypt_status (crypt, status)) {
      _status_to_error (status, error);
      mongocrypt_status_destroy (status);
      return false;
   } else if (error_expected) {
      bson_set_error (error,
                      MONGOC_ERROR_CLIENT,
                      MONGOC_ERROR_CLIENT_INVALID_ENCRYPTION_STATE,
                      "generic error from libmongocrypt handle");
      mongocrypt_status_destroy (status);
      return false;
   }
   mongocrypt_status_destroy (status);
   return true;
}

/* Convert a mongocrypt_binary_t to a static bson_t */
static bool
_bin_to_static_bson (mongocrypt_binary_t *bin, bson_t *out, bson_error_t *error)
{
   /* Copy bin into bson_t result. */
   if (!bson_init_static (
          out, mongocrypt_binary_data (bin), mongocrypt_binary_len (bin))) {
      bson_set_error (error,
                      MONGOC_ERROR_BSON,
                      MONGOC_ERROR_BSON_INVALID,
                      "invalid returned bson");
      return false;
   }
   return true;
}


typedef struct {
   mongocrypt_ctx_t *ctx;
   mongoc_collection_t *keyvault_coll;
   mongoc_client_t *mongocryptd_client;
   mongoc_client_t *collinfo_client;
   const char *db_name;
} _state_machine_t;

_state_machine_t * _state_machine_new (void) {
   return  bson_malloc0 (sizeof (_state_machine_t));
}

void _state_machine_destroy (_state_machine_t* state_machine) {
   bson_free (state_machine);
}

/* State handler MONGOCRYPT_CTX_NEED_MONGO_COLLINFO */
static bool
_state_need_mongo_collinfo (_state_machine_t *state_machine,
                            bson_error_t *error)
{
   mongoc_database_t *db = NULL;
   mongoc_cursor_t *cursor = NULL;
   bson_t filter_bson;
   const bson_t *collinfo_bson = NULL;
   bson_t opts = BSON_INITIALIZER;
   mongocrypt_binary_t *filter_bin = NULL;
   mongocrypt_binary_t *collinfo_bin = NULL;
   bool ret = false;

   /* 1. Run listCollections on the encrypted MongoClient with the filter
    * provided by mongocrypt_ctx_mongo_op */
   filter_bin = mongocrypt_binary_new ();
   if (!mongocrypt_ctx_mongo_op (state_machine->ctx, filter_bin)) {
      _ctx_check_error (state_machine->ctx, error, true);
      goto fail;
   }

   if (!_bin_to_static_bson (filter_bin, &filter_bson, error)) {
      goto fail;
   }

   bson_append_document (&opts, "filter", -1, &filter_bson);
   db = mongoc_client_get_database (state_machine->collinfo_client, state_machine->db_name);
   cursor = mongoc_database_find_collections_with_opts (db, &opts);
   if (mongoc_cursor_error (cursor, error)) {
      goto fail;
   }

   /* 2. Return the first result (if any) with mongocrypt_ctx_mongo_feed or
    * proceed to the next step if nothing was returned. */
   if (mongoc_cursor_next (cursor, &collinfo_bson)) {
      collinfo_bin = mongocrypt_binary_new_from_data (
         (uint8_t *) bson_get_data (collinfo_bson), collinfo_bson->len);
      if (!mongocrypt_ctx_mongo_feed (state_machine->ctx, collinfo_bin)) {
         _ctx_check_error (state_machine->ctx, error, true);
         goto fail;
      }
   } else if (mongoc_cursor_error (cursor, error)) {
      goto fail;
   }

   /* 3. Call mongocrypt_ctx_mongo_done */
   if (!mongocrypt_ctx_mongo_done (state_machine->ctx)) {
      _ctx_check_error (state_machine->ctx, error, true);
      goto fail;
   }

   ret = true;

fail:

   bson_destroy (&opts);
   mongocrypt_binary_destroy (filter_bin);
   mongocrypt_binary_destroy (collinfo_bin);
   mongoc_cursor_destroy (cursor);
   mongoc_database_destroy (db);
   return ret;
}

static bool
_state_need_mongo_markings (_state_machine_t *state_machine,
                            bson_error_t *error)
{
   bool ret = false;
   mongocrypt_binary_t *mongocryptd_cmd_bin = NULL;
   mongocrypt_binary_t *mongocryptd_reply_bin = NULL;
   bson_t mongocryptd_cmd_bson;
   bson_t reply = BSON_INITIALIZER;

   mongocryptd_cmd_bin = mongocrypt_binary_new ();

   if (!mongocrypt_ctx_mongo_op (state_machine->ctx, mongocryptd_cmd_bin)) {
      _ctx_check_error (state_machine->ctx, error, true);
      goto fail;
   }

   if (!_bin_to_static_bson (
          mongocryptd_cmd_bin, &mongocryptd_cmd_bson, error)) {
      goto fail;
   }

   /* 1. Use db.runCommand to run the command provided by
    * mongocrypt_ctx_mongo_op on the MongoClient connected to mongocryptd. */
   bson_destroy (&reply);
   if (!mongoc_client_command_simple (state_machine->mongocryptd_client,
                                      "admin",
                                      &mongocryptd_cmd_bson,
                                      NULL /* read_prefs */,
                                      &reply,
                                      error)) {
      _prefix_mongocryptd_error (error);
      goto fail;
   }

   /* 2. Feed the reply back with mongocrypt_ctx_mongo_feed. */
   mongocryptd_reply_bin = mongocrypt_binary_new_from_data (
      (uint8_t *) bson_get_data (&reply), reply.len);
   if (!mongocrypt_ctx_mongo_feed (state_machine->ctx, mongocryptd_reply_bin)) {
      _ctx_check_error (state_machine->ctx, error, true);
      goto fail;
   }

   /* 3. Call mongocrypt_ctx_mongo_done. */
   if (!mongocrypt_ctx_mongo_done (state_machine->ctx)) {
      _ctx_check_error (state_machine->ctx, error, true);
      goto fail;
   }

   ret = true;
fail:
   bson_destroy (&reply);
   mongocrypt_binary_destroy (mongocryptd_cmd_bin);
   mongocrypt_binary_destroy (mongocryptd_reply_bin);
   return ret;
}

static bool
_state_need_mongo_keys (_state_machine_t *state_machine, bson_error_t *error)
{
   bool ret = false;
   mongocrypt_binary_t *filter_bin = NULL;
   bson_t filter_bson;
   bson_t opts = BSON_INITIALIZER;
   mongocrypt_binary_t *key_bin = NULL;
   const bson_t *key_bson;
   mongoc_cursor_t *cursor = NULL;
   mongoc_read_concern_t *rc = NULL;

   /* 1. Use MongoCollection.find on the MongoClient connected to the key vault
    * client (which may be the same as the encrypted client). Use the filter
    * provided by mongocrypt_ctx_mongo_op. */
   filter_bin = mongocrypt_binary_new ();
   if (!mongocrypt_ctx_mongo_op (state_machine->ctx, filter_bin)) {
      _ctx_check_error (state_machine->ctx, error, true);
      goto fail;
   }

   if (!_bin_to_static_bson (filter_bin, &filter_bson, error)) {
      _ctx_check_error (state_machine->ctx, error, true);
      goto fail;
   }

   rc = mongoc_read_concern_new ();
   mongoc_read_concern_set_level (rc, MONGOC_READ_CONCERN_LEVEL_MAJORITY);
   if (!mongoc_read_concern_append (rc, &opts)) {
      bson_set_error (error,
                      MONGOC_ERROR_BSON,
                      MONGOC_ERROR_BSON_INVALID,
                      "%s",
                      "could not set read concern");
      goto fail;
   }

   cursor = mongoc_collection_find_with_opts (
      state_machine->keyvault_coll, &filter_bson, &opts, NULL /* read prefs */);
   /* 2. Feed all resulting documents back (if any) with repeated calls to
    * mongocrypt_ctx_mongo_feed. */
   while (mongoc_cursor_next (cursor, &key_bson)) {
      mongocrypt_binary_destroy (key_bin);
      key_bin = mongocrypt_binary_new_from_data (
         (uint8_t *) bson_get_data (key_bson), key_bson->len);
      if (!mongocrypt_ctx_mongo_feed (state_machine->ctx, key_bin)) {
         _ctx_check_error (state_machine->ctx, error, true);
         goto fail;
      }
   }
   if (mongoc_cursor_error (cursor, error)) {
      _prefix_keyvault_error (error);
      goto fail;
   }

   /* 3. Call mongocrypt_ctx_mongo_done. */
   if (!mongocrypt_ctx_mongo_done (state_machine->ctx)) {
      _ctx_check_error (state_machine->ctx, error, true);
      goto fail;
   }

   ret = true;
fail:
   mongocrypt_binary_destroy (filter_bin);
   mongoc_cursor_destroy (cursor);
   mongoc_read_concern_destroy (rc);
   bson_destroy (&opts);
   mongocrypt_binary_destroy (key_bin);
   return ret;
}

static mongoc_stream_t *
_get_stream (const char *endpoint,
             int32_t connecttimeoutms,
             bson_error_t *error)
{
   mongoc_stream_t *base_stream = NULL;
   mongoc_stream_t *tls_stream = NULL;
   bool ret = false;
   mongoc_ssl_opt_t ssl_opts = {0};
   mongoc_host_list_t host;
   char *copied_endpoint = NULL;

   if (!strchr (endpoint, ':')) {
      copied_endpoint = bson_strdup_printf ("%s:443", endpoint);
   }

   if (!_mongoc_host_list_from_string_with_err (
          &host, copied_endpoint ? copied_endpoint : endpoint, error)) {
      goto fail;
   }

   base_stream = mongoc_client_connect_tcp (connecttimeoutms, &host, error);
   if (!base_stream) {
      goto fail;
   }

   /* Wrap in a tls_stream. */
   memcpy (&ssl_opts, mongoc_ssl_opt_get_default (), sizeof ssl_opts);
   tls_stream = mongoc_stream_tls_new_with_hostname (
      base_stream, endpoint, &ssl_opts, 1 /* client */);

   if (!mongoc_stream_tls_handshake_block (
          tls_stream, endpoint, connecttimeoutms, error)) {
      goto fail;
   }

   ret = true;
fail:
   bson_free (copied_endpoint);
   if (!ret) {
      if (tls_stream) {
         /* destroys base_stream too */
         mongoc_stream_destroy (tls_stream);
      } else if (base_stream) {
         mongoc_stream_destroy (base_stream);
      }
      return NULL;
   }
   return tls_stream;
}

static bool
_state_need_kms (_state_machine_t *state_machine, bson_error_t *error)
{
   mongocrypt_kms_ctx_t *kms_ctx = NULL;
   mongoc_stream_t *tls_stream = NULL;
   bool ret = false;
   mongocrypt_binary_t *http_req = NULL;
   mongocrypt_binary_t *http_reply = NULL;
   const char *endpoint;
   uint32_t sockettimeout;
   
   sockettimeout = state_machine->keyvault_coll->client->cluster.sockettimeoutms;
   kms_ctx = mongocrypt_ctx_next_kms_ctx (state_machine->ctx);
   while (kms_ctx) {
      mongoc_iovec_t iov;

      mongocrypt_binary_destroy (http_req);
      http_req = mongocrypt_binary_new ();
      if (!mongocrypt_kms_ctx_message (kms_ctx, http_req)) {
         _kms_ctx_check_error (kms_ctx, error, true);
         goto fail;
      }

      if (!mongocrypt_kms_ctx_endpoint (kms_ctx, &endpoint)) {
         _kms_ctx_check_error (kms_ctx, error, true);
         goto fail;
      }

      tls_stream =
         _get_stream (endpoint,
                      sockettimeout,
                      error);
      if (!tls_stream) {
         goto fail;
      }

      iov.iov_base = (char *) mongocrypt_binary_data (http_req);
      iov.iov_len = mongocrypt_binary_len (http_req);

      if (!_mongoc_stream_writev_full (
             tls_stream,
             &iov,
             1,
             sockettimeout,
             error)) {
         goto fail;
      }

      /* Read and feed reply. */
      while (mongocrypt_kms_ctx_bytes_needed (kms_ctx) > 0) {
#define BUFFER_SIZE 1024
         uint8_t buf[BUFFER_SIZE];
         uint32_t bytes_needed = mongocrypt_kms_ctx_bytes_needed (kms_ctx);
         ssize_t read_ret;

         /* Cap the bytes requested at the buffer size. */
         if (bytes_needed > BUFFER_SIZE) {
            bytes_needed = BUFFER_SIZE;
         }

         read_ret = mongoc_stream_read (
            tls_stream,
            buf,
            bytes_needed,
            1 /* min_bytes. */,
            sockettimeout);
         if (read_ret == -1) {
            bson_set_error (error,
                            MONGOC_ERROR_STREAM,
                            MONGOC_ERROR_STREAM_SOCKET,
                            "failed to read from KMS stream: %d",
                            errno);
            goto fail;
         }

         if (read_ret == 0) {
            bson_set_error (error,
                            MONGOC_ERROR_STREAM,
                            MONGOC_ERROR_STREAM_SOCKET,
                            "unexpected EOF from KMS stream");
            goto fail;
         }

         mongocrypt_binary_destroy (http_reply);
         http_reply = mongocrypt_binary_new_from_data (buf, read_ret);
         if (!mongocrypt_kms_ctx_feed (kms_ctx, http_reply)) {
            _kms_ctx_check_error (kms_ctx, error, true);
            goto fail;
         }
      }
      kms_ctx = mongocrypt_ctx_next_kms_ctx (state_machine->ctx);
   }
   /* When NULL is returned by mongocrypt_ctx_next_kms_ctx, this can either be
    * an error or end-of-list. */
   if (!_ctx_check_error (state_machine->ctx, error, false)) {
      goto fail;
   }

   if (!mongocrypt_ctx_kms_done (state_machine->ctx)) {
      _ctx_check_error (state_machine->ctx, error, true);
      goto fail;
   }

   ret = true;
fail:
   mongoc_stream_destroy (tls_stream);
   mongocrypt_binary_destroy (http_req);
   mongocrypt_binary_destroy (http_reply);
   return ret;
#undef BUFFER_SIZE
}

static bool
_state_ready (_state_machine_t *state_machine,
              bson_t *result,
              bson_error_t *error)
{
   mongocrypt_binary_t *result_bin = NULL;
   bson_t tmp;
   bool ret = false;

   bson_init (result);
   result_bin = mongocrypt_binary_new ();
   if (!mongocrypt_ctx_finalize (state_machine->ctx, result_bin)) {
      _ctx_check_error (state_machine->ctx, error, true);
      goto fail;
   }

   if (!_bin_to_static_bson (result_bin, &tmp, error)) {
      goto fail;
   }

   bson_destroy (result);
   bson_copy_to (&tmp, result);

   ret = true;
fail:
   mongocrypt_binary_destroy (result_bin);
   return ret;
}

/*--------------------------------------------------------------------------
 *
 * _mongoc_cse_run_state_machine --
 *    Run the mongocrypt_ctx state machine.
 *
 * Post-conditions:
 *    *result may be set to a new bson_t, or NULL otherwise. Caller should
 *    not assume return value of true means *result is set. If false returned,
 *    @error is set.
 *
 * --------------------------------------------------------------------------
 */
bool
_state_machine_run (_state_machine_t *state_machine,
                    bson_t *result,
                    bson_error_t *error)
{
   bool ret = false;
   mongocrypt_binary_t *bin = NULL;

   bson_init (result);
   while (true) {
      switch (mongocrypt_ctx_state (state_machine->ctx)) {
      default:
      case MONGOCRYPT_CTX_ERROR:
         _ctx_check_error (state_machine->ctx, error, true);
         goto fail;
      case MONGOCRYPT_CTX_NEED_MONGO_COLLINFO:
         if (!_state_need_mongo_collinfo (state_machine, error)) {
            goto fail;
         }
         break;
      case MONGOCRYPT_CTX_NEED_MONGO_MARKINGS:
         if (!_state_need_mongo_markings (state_machine, error)) {
            goto fail;
         }
         break;
      case MONGOCRYPT_CTX_NEED_MONGO_KEYS:
         if (!_state_need_mongo_keys (state_machine, error)) {
            goto fail;
         }
         break;
      case MONGOCRYPT_CTX_NEED_KMS:
         if (!_state_need_kms (state_machine, error)) {
            goto fail;
         }
         break;
      case MONGOCRYPT_CTX_READY:
         bson_destroy (result);
         if (!_state_ready (state_machine, result, error)) {
            goto fail;
         }
         break;
      case MONGOCRYPT_CTX_DONE:
         goto success;
         break;
      }
   }

success:
   ret = true;
fail:
   mongocrypt_binary_destroy (bin);
   return ret;
}

/* Note, _mongoc_crypt_t only has one member, to the top-level handle of
   libmongocrypt, mongocrypt_t.
   The purpose of defining _mongoc_crypt_t is to limit all interaction with
   libmongocrypt to this one
   file.
*/
_mongoc_crypt_t *
_mongoc_crypt_new (const bson_t *kms_providers,
                   const bson_t *schema_map,
                   bson_error_t *error)
{
   _mongoc_crypt_t *crypt;
   mongocrypt_binary_t *local_masterkey_bin = NULL;
   mongocrypt_binary_t *schema_map_bin = NULL;
   bson_iter_t iter;
   bool success = false;

   /* Create the handle to libmongocrypt. */
   crypt = bson_malloc0 (sizeof (*crypt));
   crypt->handle = mongocrypt_new ();

   mongocrypt_setopt_log_handler (
      crypt->handle, _log_callback, NULL /* context */);

   /* Take options from the kms_providers map. */
   if (bson_iter_init_find (&iter, kms_providers, "aws")) {
      bson_iter_t subiter;
      const char *aws_access_key_id = NULL;
      uint32_t aws_access_key_id_len = 0;
      const char *aws_secret_access_key = NULL;
      uint32_t aws_secret_access_key_len = 0;

      if (!BSON_ITER_HOLDS_DOCUMENT (&iter)) {
         bson_set_error (error,
                         MONGOC_ERROR_CLIENT,
                         MONGOC_ERROR_CLIENT_INVALID_ENCRYPTION_ARG,
                         "Expected document for KMS provider 'aws'");
         goto fail;
      }

      BSON_ASSERT (bson_iter_recurse (&iter, &subiter));
      if (bson_iter_find (&subiter, "accessKeyId")) {
         aws_access_key_id = bson_iter_utf8 (&subiter, &aws_access_key_id_len);
      }

      BSON_ASSERT (bson_iter_recurse (&iter, &subiter));
      if (bson_iter_find (&subiter, "secretAccessKey")) {
         aws_secret_access_key =
            bson_iter_utf8 (&subiter, &aws_secret_access_key_len);
      }

      /* libmongocrypt returns error if options are NULL. */
      if (!mongocrypt_setopt_kms_provider_aws (crypt->handle,
                                               aws_access_key_id,
                                               aws_access_key_id_len,
                                               aws_secret_access_key,
                                               aws_secret_access_key_len)) {
         _crypt_check_error (crypt->handle, error, true);
         goto fail;
      }
   }

   if (bson_iter_init_find (&iter, kms_providers, "local")) {
      bson_iter_t subiter;

      if (!BSON_ITER_HOLDS_DOCUMENT (&iter)) {
         bson_set_error (error,
                         MONGOC_ERROR_CLIENT,
                         MONGOC_ERROR_CLIENT_INVALID_ENCRYPTION_ARG,
                         "Expected document for KMS provider 'local'");
         goto fail;
      }

      bson_iter_recurse (&iter, &subiter);
      if (bson_iter_find (&subiter, "key")) {
         uint32_t key_len;
         const uint8_t *key_data;

         bson_iter_binary (&subiter, NULL /* subtype */, &key_len, &key_data);
         local_masterkey_bin =
            mongocrypt_binary_new_from_data ((uint8_t *) key_data, key_len);
      }

      /* libmongocrypt returns error if options are NULL. */
      if (!mongocrypt_setopt_kms_provider_local (crypt->handle,
                                                 local_masterkey_bin)) {
         _crypt_check_error (crypt->handle, error, true);
         goto fail;
      }
   }

   if (schema_map) {
      schema_map_bin = mongocrypt_binary_new_from_data (
         (uint8_t *) bson_get_data (schema_map), schema_map->len);
      if (!mongocrypt_setopt_schema_map (crypt->handle, schema_map_bin)) {
         _crypt_check_error (crypt->handle, error, true);
         goto fail;
      }
   }

   if (!mongocrypt_init (crypt->handle)) {
      _crypt_check_error (crypt->handle, error, true);
      goto fail;
   }

   success = true;
fail:
   mongocrypt_binary_destroy (local_masterkey_bin);
   mongocrypt_binary_destroy (schema_map_bin);

   if (!success) {
      _mongoc_crypt_destroy (crypt);
      return NULL;
   }

   return crypt;
}

void
_mongoc_crypt_destroy (_mongoc_crypt_t *crypt)
{
   if (!crypt) {
      return;
   }
   mongocrypt_destroy (crypt->handle);
   bson_free (crypt);
}

bool
_mongoc_crypt_auto_encrypt (_mongoc_crypt_t *crypt,
                            mongoc_collection_t *keyvault_coll,
                            mongoc_client_t *mongocryptd_client,
                            mongoc_client_t *collinfo_client,
                            const char *db_name,
                            const bson_t *cmd_in,
                            bson_t *cmd_out,
                            bson_error_t *error)
{
   _state_machine_t *state_machine = NULL;
   mongocrypt_binary_t *cmd_bin = NULL;

   bson_init (cmd_out);

   state_machine = _state_machine_new ();
   state_machine->keyvault_coll = keyvault_coll;
   state_machine->mongocryptd_client = mongocryptd_client;
   state_machine->collinfo_client = collinfo_client;
   state_machine->db_name = db_name;
   state_machine->ctx = mongocrypt_ctx_new (crypt->handle);
   if (!state_machine->ctx) {
      _crypt_check_error (crypt->handle, error, true);
      goto fail;
   }

   cmd_bin =
      mongocrypt_binary_new_from_data ((uint8_t*)bson_get_data (cmd_in), cmd_in->len);
   if (!mongocrypt_ctx_encrypt_init (
          state_machine->ctx, db_name, -1, cmd_bin)) {
      goto fail;
   }

   bson_destroy (cmd_out);
   if (!_state_machine_run (state_machine, cmd_out, error)) {
      goto fail;
   }

fail:
   mongocrypt_binary_destroy (cmd_bin);
   _state_machine_destroy (state_machine);
   return false;
}

bool
_mongoc_crypt_auto_decrypt (_mongoc_crypt_t *crypt,
                            mongoc_collection_t *keyvault_coll,
                            const bson_t *doc_in,
                            bson_t *doc_out,
                            bson_error_t *error)
{
   _state_machine_t *state_machine = NULL;
   mongocrypt_binary_t *doc_bin = NULL;

   bson_init (doc_out);

   state_machine = _state_machine_new ();
   state_machine->keyvault_coll = keyvault_coll;
   state_machine->ctx = mongocrypt_ctx_new (crypt->handle);
   if (!state_machine->ctx) {
      _crypt_check_error (crypt->handle, error, true);
      goto fail;
   }

   doc_bin =
      mongocrypt_binary_new_from_data ((uint8_t*)bson_get_data (doc_in), doc_in->len);
   if (!mongocrypt_ctx_decrypt_init (state_machine->ctx, doc_bin)) {
      goto fail;
   }

   bson_destroy (doc_out);
   if (!_state_machine_run (state_machine, doc_out, error)) {
      goto fail;
   }

fail:
   mongocrypt_binary_destroy (doc_bin);
   _state_machine_destroy (state_machine);
   return false;
}

bool
_mongoc_crypt_explicit_encrypt (_mongoc_crypt_t *crypt,
                                mongoc_collection_t *keyvault_coll,
                                const char *algorithm,
                                const bson_value_t *keyid,
                                char *keyaltname,
                                const bson_value_t *value_in,
                                bson_value_t *value_out,
                                bson_error_t *error)
{
   _state_machine_t *state_machine = NULL;
   bson_t *to_encrypt_doc = NULL;
   mongocrypt_binary_t *to_encrypt_bin = NULL;
   bson_iter_t iter;
   bool ret = false;
   bson_t result = BSON_INITIALIZER;

   value_out->value_type = BSON_TYPE_EOD;

   /* Create the context for the operation. */
   state_machine = _state_machine_new ();
   state_machine->keyvault_coll = keyvault_coll;
   state_machine->ctx = mongocrypt_ctx_new (crypt->handle);
   if (!state_machine->ctx) {
      _crypt_check_error (crypt->handle, error, true);
      goto fail;
   }

   if (!mongocrypt_ctx_setopt_algorithm (state_machine->ctx, algorithm, -1)) {
      _ctx_check_error (state_machine->ctx, error, true);
      goto fail;
   }

   if (keyaltname) {
      bool keyaltname_ret;
      mongocrypt_binary_t *keyaltname_bin;
      bson_t *keyaltname_doc;

      keyaltname_doc = BCON_NEW ("keyAltName", keyaltname);
      keyaltname_bin = mongocrypt_binary_new_from_data (
         (uint8_t *) bson_get_data (keyaltname_doc), keyaltname_doc->len);
      keyaltname_ret = mongocrypt_ctx_setopt_key_alt_name (state_machine->ctx,
                                                           keyaltname_bin);
      mongocrypt_binary_destroy (keyaltname_bin);
      bson_destroy (keyaltname_doc);
      if (!keyaltname_ret) {
         _ctx_check_error (state_machine->ctx, error, true);
         goto fail;
      }
   }

   if (keyid &&  keyid->value_type == BSON_TYPE_BINARY) {
      mongocrypt_binary_t *keyid_bin;
      bool keyid_ret;

      if (keyid->value.v_binary.subtype != BSON_SUBTYPE_UUID) {
         bson_set_error (error,
                         MONGOC_ERROR_CLIENT,
                         MONGOC_ERROR_CLIENT_INVALID_ENCRYPTION_ARG,
                         "keyid must be a UUID");
         goto fail;
      }

      keyid_bin = mongocrypt_binary_new_from_data (
         keyid->value.v_binary.data, keyid->value.v_binary.data_len);
      keyid_ret = mongocrypt_ctx_setopt_key_id (state_machine->ctx, keyid_bin);
      mongocrypt_binary_destroy (keyid_bin);
      if (!keyid_ret) {
         _ctx_check_error (state_machine->ctx, error, true);
         goto fail;
      }
   }

   to_encrypt_doc = bson_new ();
   BSON_APPEND_VALUE (to_encrypt_doc, "v", value_in);
   to_encrypt_bin = mongocrypt_binary_new_from_data (
      (uint8_t *) bson_get_data (to_encrypt_doc), to_encrypt_doc->len);
   if (!mongocrypt_ctx_explicit_encrypt_init (state_machine->ctx,
                                              to_encrypt_bin)) {
      _ctx_check_error (state_machine->ctx, error, true);
      goto fail;
   }

   bson_destroy (&result);
   if (!_state_machine_run (
          state_machine, &result, error)) {
      goto fail;
   }

   /* extract value */
   if (!bson_iter_init_find (&iter, &result, "v")) {
      bson_set_error (error,
                      MONGOC_ERROR_CLIENT,
                      MONGOC_ERROR_CLIENT_INVALID_ENCRYPTION_STATE,
                      "encrypted result unexpected");
      goto fail;
   } else {
      const bson_value_t *tmp;

      tmp = bson_iter_value (&iter);
      bson_value_copy (tmp, value_out);
   }

   ret = true;
fail:
   _state_machine_destroy (state_machine);
   mongocrypt_binary_destroy (to_encrypt_bin);
   bson_destroy (to_encrypt_doc);
   bson_destroy (&result);
   return ret;
}

bool
_mongoc_crypt_explicit_decrypt (_mongoc_crypt_t *crypt,
                                mongoc_collection_t *keyvault_coll,
                                const bson_value_t *value_in,
                                bson_value_t *value_out,
                                bson_error_t *error)
{
   _state_machine_t *state_machine = NULL;
   bson_t *to_decrypt_doc = NULL;
   mongocrypt_binary_t *to_decrypt_bin = NULL;
   bson_iter_t iter;
   bool ret = false;
   bson_t result = BSON_INITIALIZER;

   state_machine = _state_machine_new ();
   state_machine->keyvault_coll = keyvault_coll;
   state_machine->ctx = mongocrypt_ctx_new (crypt->handle);
   if (!state_machine->ctx) {
      _crypt_check_error (crypt->handle, error, true);
      goto fail;
   }

   to_decrypt_doc = bson_new ();
   BSON_APPEND_VALUE (to_decrypt_doc, "v", value_in);
   to_decrypt_bin = mongocrypt_binary_new_from_data (
      (uint8_t *) bson_get_data (to_decrypt_doc), to_decrypt_doc->len);
   if (!mongocrypt_ctx_explicit_decrypt_init (state_machine->ctx,
                                              to_decrypt_bin)) {
      _ctx_check_error (state_machine->ctx, error, true);
      goto fail;
   }

   if (!_state_machine_run (state_machine, &result, error)) {
      goto fail;
   }

   /* extract value */
   if (!bson_iter_init_find (&iter, &result, "v")) {
      bson_set_error (error,
                      MONGOC_ERROR_CLIENT,
                      MONGOC_ERROR_CLIENT_INVALID_ENCRYPTION_STATE,
                      "decrypted result unexpected");
      goto fail;
   } else {
      const bson_value_t *tmp;

      tmp = bson_iter_value (&iter);
      bson_value_copy (tmp, value_out);
   }

   ret = true;
fail:
   _state_machine_destroy (state_machine);
   mongocrypt_binary_destroy (to_decrypt_bin);
   bson_destroy (to_decrypt_doc);
   bson_destroy (&result);
   return ret;
}

bool
_mongoc_crypt_create_datakey (_mongoc_crypt_t *crypt,
                              const char *kms_provider,
                              const bson_t *masterkey,
                              char **keyaltnames,
                              uint32_t keyaltnames_count,
                              bson_t *doc_out,
                              bson_error_t *error)
{
   _state_machine_t *state_machine = NULL;
   bool ret = false;

   bson_init (doc_out);
   state_machine->ctx = mongocrypt_ctx_new (crypt->handle);
   if (!state_machine->ctx) {
      _crypt_check_error (crypt->handle, error, true);
      goto fail;
   }

   if (0 == strcmp ("aws", kms_provider) && masterkey) {
      bson_iter_t iter;
      const char *region = NULL;
      uint32_t region_len = 0;
      const char *key = NULL;
      uint32_t key_len = 0;

      if (bson_iter_init_find (&iter, masterkey, "region") &&
          BSON_ITER_HOLDS_UTF8 (&iter)) {
         region = bson_iter_utf8 (&iter, &region_len);
      }

      if (bson_iter_init_find (&iter, masterkey, "key") &&
          BSON_ITER_HOLDS_UTF8 (&iter)) {
         key = bson_iter_utf8 (&iter, &key_len);
      }

      if (!mongocrypt_ctx_setopt_masterkey_aws (
             state_machine->ctx, region, region_len, key, key_len)) {
         _ctx_check_error (state_machine->ctx, error, true);
         goto fail;
      }

      /* Check for optional endpoint */
      if (bson_iter_init_find (&iter, masterkey, "endpoint") &&
          BSON_ITER_HOLDS_UTF8 (&iter)) {
         const char *endpoint = NULL;
         uint32_t endpoint_len = 0;

         endpoint = bson_iter_utf8 (&iter, &endpoint_len);
         if (!mongocrypt_ctx_setopt_masterkey_aws_endpoint (
                state_machine->ctx, endpoint, endpoint_len)) {
            _ctx_check_error (state_machine->ctx, error, true);
            goto fail;
         }
      }
   }

   if (0 == strcmp ("local", kms_provider)) {
      if (!mongocrypt_ctx_setopt_masterkey_local (state_machine->ctx)) {
         _ctx_check_error (state_machine->ctx, error, true);
         goto fail;
      }
   }


   if (keyaltnames) {
      int i;

      for (i = 0; i < keyaltnames_count; i++) {
         bool keyaltname_ret;
         mongocrypt_binary_t *keyaltname_bin;
         bson_t *keyaltname_doc;

         keyaltname_doc = BCON_NEW ("keyAltName", keyaltnames[i]);
         keyaltname_bin = mongocrypt_binary_new_from_data (
            (uint8_t *) bson_get_data (keyaltname_doc), keyaltname_doc->len);
         keyaltname_ret = mongocrypt_ctx_setopt_key_alt_name (
            state_machine->ctx, keyaltname_bin);
         mongocrypt_binary_destroy (keyaltname_bin);
         bson_destroy (keyaltname_doc);
         if (!keyaltname_ret) {
            _ctx_check_error (state_machine->ctx, error, true);
            goto fail;
         }
      }
   }

   if (!mongocrypt_ctx_datakey_init (state_machine->ctx)) {
      _ctx_check_error (state_machine->ctx, error, true);
      goto fail;
   }

   bson_destroy (doc_out);
   if (!_state_machine_run (state_machine, doc_out, error)) {
      goto fail;
   }

   ret = true;

fail:
   _state_machine_destroy (state_machine);
   return ret;
}

#endif /* MONGOC_ENABLE_CLIENT_SIDE_ENCRYPTION */