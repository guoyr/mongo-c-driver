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

#include "mongoc/mongoc-client-side-encryption-private.h"
#include "mongoc/mongoc-topology-private.h"

#ifndef _WIN32
#include <sys/wait.h>
#include <signal.h>
#endif

#include "mongoc/mongoc.h"

/* Auto encryption opts. */
struct _mongoc_auto_encryption_opts_t {
   /* key_vault_client and key_vault_client_pool is not owned and must outlive
    * auto encrypted client/pool. */
   mongoc_client_t *key_vault_client;
   mongoc_client_pool_t *key_vault_client_pool;
   char *db;
   char *coll;
   bson_t *kms_providers;
   bson_t *schema_map;
   bool bypass_auto_encryption;
   bson_t *extra;
};

mongoc_auto_encryption_opts_t *
mongoc_auto_encryption_opts_new (void)
{
   return bson_malloc0 (sizeof (mongoc_auto_encryption_opts_t));
}

void
mongoc_auto_encryption_opts_destroy (mongoc_auto_encryption_opts_t *opts)
{
   if (!opts) {
      return;
   }
   bson_destroy (opts->extra);
   bson_destroy (opts->kms_providers);
   bson_destroy (opts->schema_map);
   bson_free (opts->db);
   bson_free (opts->coll);
   bson_free (opts);
}

void
mongoc_auto_encryption_opts_set_key_vault_client (
   mongoc_auto_encryption_opts_t *opts, mongoc_client_t *client)
{
   /* Does not own. */
   opts->key_vault_client = client;
}

void
mongoc_auto_encryption_opts_set_key_vault_client_pool (
   mongoc_auto_encryption_opts_t *opts, mongoc_client_pool_t *pool)
{
   /* Does not own. */
   opts->key_vault_client_pool = pool;
}

void
mongoc_auto_encryption_opts_set_key_vault_namespace (
   mongoc_auto_encryption_opts_t *opts, const char *db, const char *coll)
{
   bson_free (opts->db);
   opts->db = bson_strdup (db);
   bson_free (opts->coll);
   opts->coll = bson_strdup (coll);
}

void
mongoc_auto_encryption_opts_set_kms_providers (
   mongoc_auto_encryption_opts_t *opts, const bson_t *providers)
{
   bson_destroy (opts->kms_providers);
   opts->kms_providers = NULL;
   if (providers) {
      opts->kms_providers = bson_copy (providers);
   }
}

void
mongoc_auto_encryption_opts_set_schema_map (mongoc_auto_encryption_opts_t *opts,
                                            const bson_t *schema_map)
{
   bson_destroy (opts->schema_map);
   opts->schema_map = NULL;
   if (schema_map) {
      opts->schema_map = bson_copy (schema_map);
   }
}

void
mongoc_auto_encryption_opts_set_bypass_auto_encryption (
   mongoc_auto_encryption_opts_t *opts, bool bypass_auto_encryption)
{
   opts->bypass_auto_encryption = bypass_auto_encryption;
}

void
mongoc_auto_encryption_opts_set_extra (mongoc_auto_encryption_opts_t *opts,
                                       const bson_t *extra)
{
   bson_destroy (opts->extra);
   opts->extra = NULL;
   if (extra) {
      opts->extra = bson_copy (extra);
   }
}


#ifndef MONGOC_ENABLE_CLIENT_SIDE_ENCRYPTION

bool
_mongoc_cse_auto_encrypt (mongoc_client_t *client,
                          const mongoc_cmd_t *cmd,
                          mongoc_cmd_t *encrypted_cmd,
                          bson_t *encrypted,
                          bson_error_t *error)
{
   bson_init (encrypted);
   bson_set_error (error,
                   MONGOC_ERROR_CLIENT,
                   MONGOC_ERROR_CLIENT_INVALID_ENCRYPTION_STATE,
                   "libmongoc is not built with support for Client-Side Field "
                   "Level Encryption. Configure with "
                   "ENABLE_CLIENT_SIDE_ENCRYPTION=ON.");
   return false;
}

bool
_mongoc_cse_auto_decrypt (mongoc_client_t *client,
                          const char *db_name,
                          const bson_t *reply,
                          bson_t *decrypted,
                          bson_error_t *error)
{
   bson_init (decrypted);
   bson_set_error (error,
                   MONGOC_ERROR_CLIENT,
                   MONGOC_ERROR_CLIENT_INVALID_ENCRYPTION_STATE,
                   "libmongoc is not built with support for Client-Side Field "
                   "Level Encryption. Configure with "
                   "ENABLE_CLIENT_SIDE_ENCRYPTION=ON.");
   return false;
}

bool
_mongoc_cse_enable_auto_encryption (
   mongoc_client_t *client,
   mongoc_auto_encryption_opts_t *opts /* may be NULL */,
   bson_error_t *error)
{
   bson_set_error (error,
                   MONGOC_ERROR_CLIENT,
                   MONGOC_ERROR_CLIENT_INVALID_ENCRYPTION_STATE,
                   "libmongoc is not built with support for Client-Side Field "
                   "Level Encryption. Configure with "
                   "ENABLE_CLIENT_SIDE_ENCRYPTION=ON.");
   return false;
}

#else

#include <mongocrypt/mongocrypt.h>

#include "mongoc/mongoc-client-private.h"
#include "mongoc/mongoc-stream-private.h"
#include "mongoc/mongoc-host-list-private.h"
#include "mongoc/mongoc-trace-private.h"
#include "mongoc/mongoc-util-private.h"

static void
_prefix_mongocryptd_error (bson_error_t *error)
{
   char buf[sizeof (error->message)];

   bson_snprintf (buf, sizeof (buf), "mongocryptd error: %s:", error->message);
   memcpy (error->message, buf, sizeof (buf));
}

static void
_prefix_key_vault_error (bson_error_t *error)
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

/* All the bits the state machine needs. */
typedef struct {
   mongocrypt_t *crypt;
   mongoc_client_t *mongocryptd_client;
   mongoc_client_t *key_vault_client;
   mongoc_client_t *collinfo_client;
   const char *key_vault_db;
   const char *key_vault_coll;
   bool bypass_auto_encryption;
} _auto_encrypt_t;
/* TODO: rename this to "crypt" to match Java */

_auto_encrypt_t* _auto_encrypt_new (mongocrypt_t *crypt, mongoc_client_t *mongocryptd_client, mongoc_client_t* key_vault_client, mongoc_client_t* collinfo_client, const char* key_vault_db, const char *key_vault_coll, bool bypass_auto_encryption) {
   _auto_encrypt_t *auto_encrypt;

   auto_encrypt = bson_malloc0 (sizeof (*auto_encrypt));
   auto_encrypt->crypt = crypt;
   auto_encrypt->mongocryptd_client = mongocryptd_client;
   auto_encrypt->key_vault_client = key_vault_client;
   auto_encrypt->collinfo_client = collinfo_client;
   auto_encrypt->key_vault_db = key_vault_db;
   auto_encrypt->key_vault_coll = key_vault_coll;
   auto_encrypt->bypass_auto_encryption = bypass_auto_encryption;
   return auto_encrypt;
}

void _auto_encrypt_destroy (_auto_encrypt_t* auto_encrypt) {
   bson_free (auto_encrypt);
}

_auto_encrypt_t* _auto_encrypt_new_from_client (mongoc_client_t* client) {
   _auto_encrypt_t *auto_encrypt;

   auto_encrypt = bson_malloc0 (sizeof (*auto_encrypt));
   if (client->topology->single_threaded) {
      auto_encrypt->crypt = client->crypt;
      auto_encrypt->mongocryptd_client = client->mongocryptd_client;
      auto_encrypt->key_vault_client = client->key_vault_client;
      if (!auto_encrypt->key_vault_client) {
         auto_encrypt->key_vault_client = client;
      }
      auto_encrypt->collinfo_client = client;
      auto_encrypt->key_vault_db = client->key_vault_db;
      auto_encrypt->key_vault_coll = client->key_vault_coll;
      auto_encrypt->bypass_auto_encryption = client->bypass_auto_encryption;
   } else {
      auto_encrypt->crypt = client->topology->crypt;
      auto_encrypt->mongocryptd_client = mongoc_client_pool_pop (client->topology->mongocryptd_client_pool);
      if (client->topology->key_vault_client_pool) {
         auto_encrypt->key_vault_client = mongoc_client_pool_pop (client->topology->key_vault_client_pool);
      } else {
         auto_encrypt->key_vault_client = client;
      }

      auto_encrypt->collinfo_client = client;
      auto_encrypt->key_vault_db = client->topology->key_vault_db;
      auto_encrypt->key_vault_coll = client->topology->key_vault_coll;
      auto_encrypt->bypass_auto_encryption = client->topology->bypass_auto_encryption;
   }
   return auto_encrypt;
}

void _auto_encrypt_destroy_from_client (_auto_encrypt_t *auto_encrypt, mongoc_client_t* client) {
   if (!client->topology->single_threaded) {
      mongoc_client_pool_push (client->topology->mongocryptd_client_pool, auto_encrypt->mongocryptd_client);
      if (client->topology->key_vault_client_pool) {
         mongoc_client_pool_push (client->topology->key_vault_client_pool, auto_encrypt->key_vault_client);
      }
   }
   bson_free (auto_encrypt);
}

/* State handler MONGOCRYPT_CTX_NEED_MONGO_COLLINFO */
static bool
_state_need_mongo_collinfo (_auto_encrypt_t *auto_encrypt,
                            const char *db_name,
                            mongocrypt_ctx_t *ctx,
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
   if (!mongocrypt_ctx_mongo_op (ctx, filter_bin)) {
      _ctx_check_error (ctx, error, true);
      goto fail;
   }

   if (!_bin_to_static_bson (filter_bin, &filter_bson, error)) {
      goto fail;
   }

   bson_append_document (&opts, "filter", -1, &filter_bson);
   db = mongoc_client_get_database (auto_encrypt->collinfo_client, db_name);
   cursor = mongoc_database_find_collections_with_opts (db, &opts);
   if (mongoc_cursor_error (cursor, error)) {
      goto fail;
   }

   /* 2. Return the first result (if any) with mongocrypt_ctx_mongo_feed or
    * proceed to the next step if nothing was returned. */
   if (mongoc_cursor_next (cursor, &collinfo_bson)) {
      collinfo_bin = mongocrypt_binary_new_from_data (
         (uint8_t *) bson_get_data (collinfo_bson), collinfo_bson->len);
      if (!mongocrypt_ctx_mongo_feed (ctx, collinfo_bin)) {
         _ctx_check_error (ctx, error, true);
         goto fail;
      }
   } else if (mongoc_cursor_error (cursor, error)) {
      goto fail;
   }

   /* 3. Call mongocrypt_ctx_mongo_done */
   if (!mongocrypt_ctx_mongo_done (ctx)) {
      _ctx_check_error (ctx, error, true);
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
_state_need_mongo_markings (_auto_encrypt_t *auto_encrypt,
                            mongocrypt_ctx_t *ctx,
                            bson_error_t *error)
{
   bool ret = false;
   mongocrypt_binary_t *mongocryptd_cmd_bin = NULL;
   mongocrypt_binary_t *mongocryptd_reply_bin = NULL;
   bson_t mongocryptd_cmd_bson;
   bson_t reply = BSON_INITIALIZER;

   mongocryptd_cmd_bin = mongocrypt_binary_new ();

   if (!mongocrypt_ctx_mongo_op (ctx, mongocryptd_cmd_bin)) {
      _ctx_check_error (ctx, error, true);
      goto fail;
   }

   if (!_bin_to_static_bson (
          mongocryptd_cmd_bin, &mongocryptd_cmd_bson, error)) {
      goto fail;
   }

   /* 1. Use db.runCommand to run the command provided by
    * mongocrypt_ctx_mongo_op on the MongoClient connected to mongocryptd. */
   bson_destroy (&reply);
   if (!mongoc_client_command_simple (auto_encrypt->mongocryptd_client,
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
   if (!mongocrypt_ctx_mongo_feed (ctx, mongocryptd_reply_bin)) {
      _ctx_check_error (ctx, error, true);
      goto fail;
   }

   /* 3. Call mongocrypt_ctx_mongo_done. */
   if (!mongocrypt_ctx_mongo_done (ctx)) {
      _ctx_check_error (ctx, error, true);
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
_state_need_mongo_keys (_auto_encrypt_t *auto_encrypt,
                        mongocrypt_ctx_t *ctx,
                        bson_error_t *error)
{
   bool ret = false;
   mongocrypt_binary_t *filter_bin = NULL;
   bson_t filter_bson;
   bson_t opts = BSON_INITIALIZER;
   mongocrypt_binary_t *key_bin = NULL;
   const bson_t *key_bson;
   mongoc_cursor_t *cursor = NULL;
   mongoc_read_concern_t *rc = NULL;
   mongoc_collection_t *key_vault_coll = NULL;

   /* 1. Use MongoCollection.find on the MongoClient connected to the key vault
    * client (which may be the same as the encrypted client). Use the filter
    * provided by mongocrypt_ctx_mongo_op. */
   filter_bin = mongocrypt_binary_new ();
   if (!mongocrypt_ctx_mongo_op (ctx, filter_bin)) {
      _ctx_check_error (ctx, error, true);
      goto fail;
   }

   if (!_bin_to_static_bson (filter_bin, &filter_bson, error)) {
      _ctx_check_error (ctx, error, true);
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

   key_vault_coll = mongoc_client_get_collection (
      auto_encrypt->key_vault_client, auto_encrypt->key_vault_db, auto_encrypt->key_vault_coll);
   cursor = mongoc_collection_find_with_opts (
      key_vault_coll, &filter_bson, &opts, NULL /* read prefs */);
   /* 2. Feed all resulting documents back (if any) with repeated calls to
    * mongocrypt_ctx_mongo_feed. */
   while (mongoc_cursor_next (cursor, &key_bson)) {
      mongocrypt_binary_destroy (key_bin);
      key_bin = mongocrypt_binary_new_from_data (
         (uint8_t *) bson_get_data (key_bson), key_bson->len);
      if (!mongocrypt_ctx_mongo_feed (ctx, key_bin)) {
         _ctx_check_error (ctx, error, true);
         goto fail;
      }
   }
   if (mongoc_cursor_error (cursor, error)) {
      _prefix_key_vault_error (error);
      goto fail;
   }

   /* 3. Call mongocrypt_ctx_mongo_done. */
   if (!mongocrypt_ctx_mongo_done (ctx)) {
      _ctx_check_error (ctx, error, true);
      goto fail;
   }

   ret = true;
fail:
   mongocrypt_binary_destroy (filter_bin);
   mongoc_cursor_destroy (cursor);
   mongoc_read_concern_destroy (rc);
   bson_destroy (&opts);
   mongocrypt_binary_destroy (key_bin);
   mongoc_collection_destroy (key_vault_coll);
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
_state_need_kms (_auto_encrypt_t *auto_encrypt,
                 mongocrypt_ctx_t *ctx,
                 bson_error_t *error)
{
   mongocrypt_kms_ctx_t *kms_ctx = NULL;
   mongoc_stream_t *tls_stream = NULL;
   bool ret = false;
   mongocrypt_binary_t *http_req = NULL;
   mongocrypt_binary_t *http_reply = NULL;
   const char *endpoint;

   kms_ctx = mongocrypt_ctx_next_kms_ctx (ctx);
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
         _get_stream (endpoint, auto_encrypt->key_vault_client->cluster.sockettimeoutms, error);
      if (!tls_stream) {
         goto fail;
      }

      iov.iov_base = (char *) mongocrypt_binary_data (http_req);
      iov.iov_len = mongocrypt_binary_len (http_req);

      if (!_mongoc_stream_writev_full (
             tls_stream, &iov, 1, auto_encrypt->key_vault_client->cluster.sockettimeoutms, error)) {
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

         read_ret = mongoc_stream_read (tls_stream,
                                        buf,
                                        bytes_needed,
                                        1 /* min_bytes. */,
                                        auto_encrypt->key_vault_client->cluster.sockettimeoutms);
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
      kms_ctx = mongocrypt_ctx_next_kms_ctx (ctx);
   }
   /* When NULL is returned by mongocrypt_ctx_next_kms_ctx, this can either be
    * an error or end-of-list. */
   if (!_ctx_check_error (ctx, error, false)) {
      goto fail;
   }

   if (!mongocrypt_ctx_kms_done (ctx)) {
      _ctx_check_error (ctx, error, true);
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
_state_ready (mongocrypt_ctx_t *ctx,
              bson_t **result,
              bson_error_t *error)
{
   mongocrypt_binary_t *result_bin = NULL;
   bson_t tmp;
   bool ret = false;

   result_bin = mongocrypt_binary_new ();
   if (!mongocrypt_ctx_finalize (ctx, result_bin)) {
      _ctx_check_error (ctx, error, true);
      goto fail;
   }

   if (!_bin_to_static_bson (result_bin, &tmp, error)) {
      goto fail;
   }

   *result = bson_copy (&tmp);

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
_mongoc_cse_run_state_machine (_auto_encrypt_t *auto_encrypt,
                               const char *db_name,
                               mongocrypt_ctx_t *ctx,
                               bson_t **result,
                               bson_error_t *error)
{
   bool ret = false;
   mongocrypt_binary_t *bin = NULL;

   *result = NULL;
   while (true) {
      switch (mongocrypt_ctx_state (ctx)) {
      default:
      case MONGOCRYPT_CTX_ERROR:
         _ctx_check_error (ctx, error, true);
         goto fail;
      case MONGOCRYPT_CTX_NEED_MONGO_COLLINFO:
         if (!_state_need_mongo_collinfo (auto_encrypt, db_name, ctx, error)) {
            goto fail;
         }
         break;
      case MONGOCRYPT_CTX_NEED_MONGO_MARKINGS:
         if (!_state_need_mongo_markings (auto_encrypt, ctx, error)) {
            goto fail;
         }
         break;
      case MONGOCRYPT_CTX_NEED_MONGO_KEYS:
         if (!_state_need_mongo_keys (auto_encrypt, ctx, error)) {
            goto fail;
         }
         break;
      case MONGOCRYPT_CTX_NEED_KMS:
         if (!_state_need_kms (auto_encrypt, ctx, error)) {
            goto fail;
         }
         break;
      case MONGOCRYPT_CTX_READY:
         if (!_state_ready (ctx, result, error)) {
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


/*--------------------------------------------------------------------------
 *
 * _prep_for_auto_encryption --
 *    If @cmd contains a type=1 payload (document sequence), convert it into
 *    a type=0 payload (array payload). See OP_MSG spec for details.
 *    Place the command BSON that should be encrypted into @out.
 *
 * Post-conditions:
 *    @out is initialized and set to the full payload. If @cmd did not include
 *    a type=1 payload, @out is statically initialized. Caller must not modify
 *    @out after, but must call bson_destroy.
 *
 * --------------------------------------------------------------------------
 */
static void
_prep_for_auto_encryption (const mongoc_cmd_t *cmd, bson_t *out)
{
   /* If there is no type=1 payload, return the command unchanged. */
   if (!cmd->payload || !cmd->payload_size) {
      bson_init_static (out, bson_get_data (cmd->command), cmd->command->len);
      return;
   }

   /* Otherwise, append the type=1 payload as an array. */
   bson_copy_to (cmd->command, out);
   _mongoc_cmd_append_payload_as_array (cmd, out);
}

/*--------------------------------------------------------------------------
 *
 * _mongoc_cse_auto_encrypt --
 *
 *       Perform automatic encryption if enabled.
 *
 * Return:
 *       True on success, false on error.
 *
 * Pre-conditions:
 *       CSE is enabled on client or its associated client pool.
 *
 * Post-conditions:
 *       If return false, @error is set. @encrypted is always initialized.
 *       @encrypted_cmd is set to the mongoc_cmd_t to send, which may refer
 *       to @encrypted.
 *       If automatic encryption was bypassed, @encrypted is set to an empty
 *       document, but @encrypted_cmd is a copy of @cmd. Caller must always
 *       bson_destroy @encrypted.
 *
 *--------------------------------------------------------------------------
 */
bool
_mongoc_cse_auto_encrypt (mongoc_client_t *client,
                          const mongoc_cmd_t *cmd,
                          mongoc_cmd_t *encrypted_cmd,
                          bson_t *encrypted,
                          bson_error_t *error)
{
   mongocrypt_ctx_t *ctx = NULL;
   mongocrypt_binary_t *cmd_bin = NULL;
   bool ret = false;
   bson_t cmd_bson = BSON_INITIALIZER;
   bson_t *result = NULL;
   bson_iter_t iter;
   _auto_encrypt_t* auto_encrypt;

   ENTRY;

   bson_init (encrypted);
   /* TODO: push this logic into auto_encrypt class? */
   auto_encrypt = _auto_encrypt_new_from_client (client);

   if (auto_encrypt->bypass_auto_encryption) {
      memcpy (encrypted_cmd, cmd, sizeof (mongoc_cmd_t));
      bson_destroy (&cmd_bson);
      return true;
   }

   if (!auto_encrypt->bypass_auto_encryption &&
       cmd->server_stream->sd->max_wire_version < WIRE_VERSION_CSE) {
      bson_set_error (
         error,
         MONGOC_ERROR_PROTOCOL,
         MONGOC_ERROR_PROTOCOL_BAD_WIRE_VERSION,
         "%s",
         "Auto-encryption requires a minimum MongoDB version of 4.2");
      goto fail;
   }

   /* Create the context for the operation. */
   ctx = mongocrypt_ctx_new (auto_encrypt->crypt);
   if (!ctx) {
      _crypt_check_error (auto_encrypt->crypt, error, true);
      goto fail;
   }

   /* Construct the command we're sending to libmongocrypt. If cmd includes a
    * type 1 payload, convert it to a type 0 payload. */
   bson_destroy (&cmd_bson);
   _prep_for_auto_encryption (cmd, &cmd_bson);
   cmd_bin = mongocrypt_binary_new_from_data (
      (uint8_t *) bson_get_data (&cmd_bson), cmd_bson.len);
   if (!mongocrypt_ctx_encrypt_init (ctx, cmd->db_name, -1, cmd_bin)) {
      _ctx_check_error (ctx, error, true);
      goto fail;
   }

   if (!_mongoc_cse_run_state_machine (
          auto_encrypt, cmd->db_name, ctx, &result, error)) {
      goto fail;
   }

   if (result) {
      bson_destroy (encrypted);
      bson_steal (encrypted, result);
      result = NULL;
   }

   /* Re-append $db if encryption stripped it. */
   if (!bson_iter_init_find (&iter, encrypted, "$db")) {
      BSON_APPEND_UTF8 (encrypted, "$db", cmd->db_name);
   }

   /* Create the modified cmd_t. */
   memcpy (encrypted_cmd, cmd, sizeof (mongoc_cmd_t));
   /* Modify the mongoc_cmd_t and clear the payload, since
    * _mongoc_cse_auto_encrypt converted the payload into an embedded array. */
   encrypted_cmd->payload = NULL;
   encrypted_cmd->payload_size = 0;
   encrypted_cmd->command = encrypted;

   ret = true;

fail:
   bson_destroy (result);
   bson_destroy (&cmd_bson);
   mongocrypt_binary_destroy (cmd_bin);
   mongocrypt_ctx_destroy (ctx);
   _auto_encrypt_destroy_from_client (auto_encrypt, client);
   RETURN (ret);
}

/*--------------------------------------------------------------------------
 *
 * _mongoc_cse_auto_decrypt --
 *
 *       Perform automatic decryption.
 *
 * Return:
 *       True on success, false on error.
 *
 * Pre-conditions:
 *       FLE is enabled on client.
 *
 * Post-conditions:
 *       If return false, @error is set. @decrypted is always initialized.
 *
 *--------------------------------------------------------------------------
 */
bool
_mongoc_cse_auto_decrypt (mongoc_client_t *client,
                          const char *db_name,
                          const bson_t *reply,
                          bson_t *decrypted,
                          bson_error_t *error)
{
   mongocrypt_ctx_t *ctx = NULL;
   mongocrypt_binary_t *reply_bin = NULL;
   bool ret = false;
   bson_t *result = NULL;
   _auto_encrypt_t* auto_encrypt;

   ENTRY;
   bson_init (decrypted);

   auto_encrypt = _auto_encrypt_new_from_client (client);

   /* Create the context for the operation. */
   ctx = mongocrypt_ctx_new (auto_encrypt->crypt);
   if (!ctx) {
      _crypt_check_error (auto_encrypt->crypt, error, true);
      goto fail;
   }

   reply_bin = mongocrypt_binary_new_from_data (
      (uint8_t *) bson_get_data (reply), reply->len);
   if (!mongocrypt_ctx_decrypt_init (ctx, reply_bin)) {
      _ctx_check_error (ctx, error, true);
      goto fail;
   }

   if (!_mongoc_cse_run_state_machine (auto_encrypt, db_name, ctx, &result, error)) {
      goto fail;
   }

   if (result) {
      bson_destroy (decrypted);
      bson_steal (decrypted, result);
      result = NULL;
   }

   ret = true;

fail:
   bson_destroy (result);
   mongocrypt_binary_destroy (reply_bin);
   mongocrypt_ctx_destroy (ctx);
   _auto_encrypt_destroy_from_client (auto_encrypt, client);
   RETURN (ret);
}

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
_uri_construction_error (bson_error_t *error)
{
   bson_set_error (error,
                   MONGOC_ERROR_CLIENT,
                   MONGOC_ERROR_CLIENT_INVALID_ENCRYPTION_STATE,
                   "Error constructing URI to mongocryptd");
}

/* Initial state shared when enabling automatic encryption on pooled and single
 * threaded clients
 */
typedef struct {
   bool bypass_auto_encryption;
   mongoc_uri_t *mongocryptd_uri;
   bool mongocryptd_bypass_spawn;
   const char *mongocryptd_spawn_path;
   bson_iter_t mongocryptd_spawn_args;
   bool has_spawn_args;
   mongocrypt_t *crypt;
} _auto_encrypt_init_t;

static _auto_encrypt_init_t *
_auto_encrypt_init_new (void)
{
   return bson_malloc0 (sizeof (_auto_encrypt_init_t));
}

static void
_auto_encrypt_init_destroy (_auto_encrypt_init_t *auto_encrypt)
{
   if (!auto_encrypt) {
      return;
   }
   mongoc_uri_destroy (auto_encrypt->mongocryptd_uri);
   mongocrypt_destroy (auto_encrypt->crypt);
   bson_free (auto_encrypt);
}

static mongocrypt_t *
_create_mongocrypt (const bson_t* kms_providers, const bson_t* schema_map, bson_error_t* error) {
   mongocrypt_t *crypt;
   mongocrypt_binary_t *local_masterkey_bin = NULL;
   mongocrypt_binary_t *schema_map_bin = NULL;
   bson_iter_t iter;
   bool success = false;

   /* Create the handle to libmongocrypt. */
   crypt = mongocrypt_new ();

   mongocrypt_setopt_log_handler (crypt, _log_callback, NULL /* context */);

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
      if (!mongocrypt_setopt_kms_provider_aws (crypt,
                                               aws_access_key_id,
                                               aws_access_key_id_len,
                                               aws_secret_access_key,
                                               aws_secret_access_key_len)) {
         _crypt_check_error (crypt, error, true);
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
      if (!mongocrypt_setopt_kms_provider_local (crypt,
                                                 local_masterkey_bin)) {
         _crypt_check_error (crypt, error, true);
         goto fail;
      }
   }

   if (schema_map) {
      schema_map_bin = mongocrypt_binary_new_from_data (
         (uint8_t *) bson_get_data (schema_map), schema_map->len);
      if (!mongocrypt_setopt_schema_map (crypt, schema_map_bin)) {
         _crypt_check_error (crypt, error, true);
         goto fail;
      }
   }

   if (!mongocrypt_init (crypt)) {
      _crypt_check_error (crypt, error, true);
      goto fail;
   }

   success = true;
fail:
   mongocrypt_binary_destroy (local_masterkey_bin);
   mongocrypt_binary_destroy (schema_map_bin);

   if (!success) {
      mongocrypt_destroy (crypt);
      return NULL;
   }
   return crypt;
}

static bool
_auto_encrypt_init (mongoc_auto_encryption_opts_t *opts,
                    _auto_encrypt_init_t *auto_encrypt,
                    bson_error_t *error)
{
   bson_iter_t iter;
   bool ret = false;
   mongoc_uri_t *mongocryptd_uri = NULL;

   ENTRY;

   if (!opts) {
      bson_set_error (error,
                      MONGOC_ERROR_CLIENT,
                      MONGOC_ERROR_CLIENT_INVALID_ENCRYPTION_ARG,
                      "Auto encryption options required");
      goto fail;
   }

   /* Check for required options */
   if (!opts->db || !opts->coll) {
      bson_set_error (error,
                      MONGOC_ERROR_CLIENT,
                      MONGOC_ERROR_CLIENT_INVALID_ENCRYPTION_ARG,
                      "Key vault namespace option required");
      goto fail;
   }

   if (!opts->kms_providers) {
      bson_set_error (error,
                      MONGOC_ERROR_CLIENT,
                      MONGOC_ERROR_CLIENT_INVALID_ENCRYPTION_ARG,
                      "KMS providers option required");
      goto fail;
   }

   auto_encrypt->bypass_auto_encryption = opts->bypass_auto_encryption;

   if (!auto_encrypt->bypass_auto_encryption) {
      /* Spawn mongocryptd if needed, and create a client to it. */

      if (opts->extra) {
         if (bson_iter_init_find (
                &iter, opts->extra, "mongocryptdBypassSpawn") &&
             bson_iter_as_bool (&iter)) {
            auto_encrypt->mongocryptd_bypass_spawn = true;
         }
         if (bson_iter_init_find (&iter, opts->extra, "mongocryptdSpawnPath") &&
             BSON_ITER_HOLDS_UTF8 (&iter)) {
            auto_encrypt->mongocryptd_spawn_path = bson_iter_utf8 (&iter, NULL);
         }
         if (bson_iter_init_find (&iter, opts->extra, "mongocryptdSpawnArgs") &&
             BSON_ITER_HOLDS_ARRAY (&iter)) {
            memcpy (&auto_encrypt->mongocryptd_spawn_args,
                    &iter,
                    sizeof (bson_iter_t));
            auto_encrypt->has_spawn_args = true;
         }

         if (bson_iter_init_find (&iter, opts->extra, "mongocryptdURI")) {
            if (!BSON_ITER_HOLDS_UTF8 (&iter)) {
               bson_set_error (error,
                               MONGOC_ERROR_CLIENT,
                               MONGOC_ERROR_CLIENT_INVALID_ENCRYPTION_ARG,
                               "Expected string for option 'mongocryptdURI'");
               goto fail;
            }
            auto_encrypt->mongocryptd_uri =
               mongoc_uri_new_with_error (bson_iter_utf8 (&iter, NULL), error);
            if (!mongocryptd_uri) {
               goto fail;
            }
         }
      }

      if (!auto_encrypt->mongocryptd_uri) {
         /* Always default to connecting to TCP, despite spec v1.0.0. Because
          * starting mongocryptd when one is running removes the domain socket
          * file per SERVER-41029. Connecting over TCP is more reliable.
          */
         auto_encrypt->mongocryptd_uri =
            mongoc_uri_new_with_error ("mongodb://localhost:27020", error);

         if (!auto_encrypt->mongocryptd_uri) {
            goto fail;
         }

         if (!mongoc_uri_set_option_as_int32 (
                auto_encrypt->mongocryptd_uri,
                MONGOC_URI_SERVERSELECTIONTIMEOUTMS,
                5000)) {
            _uri_construction_error (error);
            goto fail;
         }
      }
   }

   auto_encrypt->crypt = _create_mongocrypt (opts->kms_providers, opts->schema_map, error);
   if (!auto_encrypt->crypt) {
      goto fail;
   }

   ret = true;
fail:
   RETURN (ret);
}


bool
_mongoc_cse_enable_auto_encryption (mongoc_client_t *client,
                                    mongoc_auto_encryption_opts_t *opts,
                                    bson_error_t *error)
{
   bool ret = false;
   _auto_encrypt_init_t *auto_encrypt = NULL;

   ENTRY;

   if (!client->topology->single_threaded) {
      bson_set_error (
         error,
         MONGOC_ERROR_CLIENT,
         MONGOC_ERROR_CLIENT_INVALID_ENCRYPTION_ARG,
         "Automatic encryption on pooled clients must be set on the pool");
      goto fail;
   }

   if (client->cse_enabled) {
      bson_set_error (error,
                      MONGOC_ERROR_CLIENT,
                      MONGOC_ERROR_CLIENT_INVALID_ENCRYPTION_STATE,
                      "Automatic encryption already set");
      goto fail;
   }

   if (opts->key_vault_client_pool) {
      bson_set_error (error,
                      MONGOC_ERROR_CLIENT,
                      MONGOC_ERROR_CLIENT_INVALID_ENCRYPTION_ARG,
                      "The key vault client pool only applies to a client "
                      "pool, not a single threaded client");
      goto fail;
   }

   auto_encrypt = _auto_encrypt_init_new ();
   if (!_auto_encrypt_init (opts, auto_encrypt, error)) {
      goto fail;
   }

   /* Steal "crypt" */
   client->crypt = auto_encrypt->crypt;
   auto_encrypt->crypt = NULL;
   client->cse_enabled = true;
   client->bypass_auto_encryption = auto_encrypt->bypass_auto_encryption;

   if (!auto_encrypt->bypass_auto_encryption) {
      if (!auto_encrypt->mongocryptd_bypass_spawn) {
         if (!_mongoc_fle_spawn_mongocryptd (
                auto_encrypt->mongocryptd_spawn_path,
                auto_encrypt->has_spawn_args
                   ? &auto_encrypt->mongocryptd_spawn_args
                   : NULL,
                error)) {
            goto fail;
         }
      }

      /* By default, single threaded clients set serverSelectionTryOnce to
       * true, which means server selection fails if a topology scan fails
       * the first time (i.e. it will not make repeat attempts until
       * serverSelectionTimeoutMS expires). Override this, since the first
       * attempt to connect to mongocryptd may fail when spawning, as it
       * takes some time for mongocryptd to listen on sockets. */
      if (!mongoc_uri_set_option_as_bool (auto_encrypt->mongocryptd_uri,
                                          MONGOC_URI_SERVERSELECTIONTRYONCE,
                                          false)) {
         _uri_construction_error (error);
         goto fail;
      }

      client->mongocryptd_client =
         mongoc_client_new_from_uri (auto_encrypt->mongocryptd_uri);

      if (!client->mongocryptd_client) {
         bson_set_error (error,
                         MONGOC_ERROR_CLIENT,
                         MONGOC_ERROR_CLIENT_INVALID_ENCRYPTION_STATE,
                         "Unable to create client to mongocryptd");
         goto fail;
      }
      /* Similarly, single threaded clients will by default wait for 5 second
       * cooldown period after failing to connect to a server before making
       * another attempt. Meaning if the first attempt to mongocryptd fails
       * to connect, then the user observes a 5 second delay. This is not
       * configurable in the URI, so override. */
      _mongoc_topology_bypass_cooldown (client->mongocryptd_client->topology);
   }

   client->key_vault_db = bson_strdup (opts->db);
   client->key_vault_coll = bson_strdup (opts->coll);
   if (opts->key_vault_client) {
      client->key_vault_client = opts->key_vault_client;
   }

   ret = true;
fail:
   _auto_encrypt_init_destroy (auto_encrypt);
   RETURN (ret);
}

bool
_mongoc_topology_cse_enable_auto_encryption (
   mongoc_topology_t *topology,
   mongoc_auto_encryption_opts_t *opts,
   bson_error_t *error)
{
   bool ret = false;
   _auto_encrypt_init_t *auto_encrypt = NULL;

   /* TODO */
   if (opts->key_vault_client) {
      bson_set_error (error,
                      MONGOC_ERROR_CLIENT,
                      MONGOC_ERROR_CLIENT_INVALID_ENCRYPTION_ARG,
                      "The key vault client only applies to a single threaded "
                      "client not a single threaded client. Set a key vault "
                      "client pool");
      goto fail;
   }

   if (topology->cse_enabled) {
      bson_set_error (error,
                      MONGOC_ERROR_CLIENT,
                      MONGOC_ERROR_CLIENT_INVALID_ENCRYPTION_STATE,
                      "Automatic encryption already set");
      goto fail;
   }

   /* TODO: lock the mutex? */

   auto_encrypt = _auto_encrypt_init_new ();
   if (!_auto_encrypt_init (opts, auto_encrypt, error)) {
      goto fail;
   }

   /* Steal "crypt" */
   topology->crypt = auto_encrypt->crypt;
   auto_encrypt->crypt = NULL;
   topology->cse_enabled = true;
   topology->bypass_auto_encryption = auto_encrypt->bypass_auto_encryption;

   if (!auto_encrypt->bypass_auto_encryption) {
      if (!auto_encrypt->mongocryptd_bypass_spawn) {
         if (!_mongoc_fle_spawn_mongocryptd (
                auto_encrypt->mongocryptd_spawn_path,
                auto_encrypt->has_spawn_args
                   ? &auto_encrypt->mongocryptd_spawn_args
                   : NULL,
                error)) {
            goto fail;
         }
      }

      topology->mongocryptd_client_pool =
         mongoc_client_pool_new (auto_encrypt->mongocryptd_uri);

      if (!topology->mongocryptd_client_pool) {
         bson_set_error (error,
                         MONGOC_ERROR_CLIENT,
                         MONGOC_ERROR_CLIENT_INVALID_ENCRYPTION_STATE,
                         "Unable to create client pool to mongocryptd");
         goto fail;
      }
   }

   topology->key_vault_db = bson_strdup (opts->db);
   topology->key_vault_coll = bson_strdup (opts->coll);
   if (opts->key_vault_client_pool) {
      topology->key_vault_client_pool = opts->key_vault_client_pool;
   }

   ret = true;
fail:
   _auto_encrypt_init_destroy (auto_encrypt);
   return ret;
}

#ifdef _WIN32
static bool
_do_spawn (const char *path, char **args, bson_error_t *error)
{
   bson_string_t *command;
   char **arg;
   PROCESS_INFORMATION process_information;
   STARTUPINFO startup_info;

   /* Construct the full command, quote path and arguments. */
   command = bson_string_new ("");
   bson_string_append (command, "\"");
   if (path) {
      bson_string_append (command, path);
   }
   bson_string_append (command, "mongocryptd.exe");
   bson_string_append (command, "\"");
   /* skip the "mongocryptd" first arg. */
   arg = args + 1;
   while (*arg) {
      bson_string_append (command, " \"");
      bson_string_append (command, *arg);
      bson_string_append (command, "\"");
      arg++;
   }

   ZeroMemory (&process_information, sizeof (process_information));
   ZeroMemory (&startup_info, sizeof (startup_info));

   startup_info.cb = sizeof (startup_info);

   if (!CreateProcessA (NULL,
                        command->str,
                        NULL,
                        NULL,
                        false /* inherit descriptors */,
                        DETACHED_PROCESS /* FLAGS */,
                        NULL /* environment */,
                        NULL /* current directory */,
                        &startup_info,
                        &process_information)) {
      long lastError = GetLastError ();
      LPSTR message = NULL;

      FormatMessageA (
         FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_ARGUMENT_ARRAY |
            FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
         NULL,
         lastError,
         0,
         (LPSTR) &message,
         0,
         NULL);

      bson_set_error (error,
                      MONGOC_ERROR_CLIENT,
                      MONGOC_ERROR_CLIENT_INVALID_ENCRYPTION_STATE,
                      "failed to spawn mongocryptd: %s",
                      message);
      LocalFree (message);
      bson_string_free (command, true);
      return false;
   }

   bson_string_free (command, true);
   return true;
}
#else

/*--------------------------------------------------------------------------
 *
 * _do_spawn --
 *
 *   Spawn process defined by arg[0] on POSIX systems.
 *
 *   Note, if mongocryptd fails to spawn (due to not being found on the path),
 *   an error is not reported and true is returned. Users will get an error
 *   later, upon first attempt to use mongocryptd.
 *
 *   These comments refer to three distinct processes: parent, child, and
 *   mongocryptd.
 *   - parent is initial calling process
 *   - child is the first forked child. It fork-execs mongocryptd then
 *     terminates. This makes mongocryptd an orphan, making it immediately
 *     adopted by the init process.
 *   - mongocryptd is the final background daemon (grandchild process).
 *
 * Return:
 *   False if an error definitely occurred. Returns true if no reportable
 *   error occurred (though an error may have occurred in starting
 *   mongocryptd, resulting in the process not running).
 *
 * Arguments:
 *    args - A NULL terminated list of arguments. The first argument MUST
 *    be the name of the process to execute, and the last argument MUST be
 *    NULL.
 *
 * Post-conditions:
 *    If return false, @error is set.
 *
 *--------------------------------------------------------------------------
 */
static bool
_do_spawn (const char *path, char **args, bson_error_t *error)
{
   pid_t pid;
   int fd;
   char *to_exec;

   /* Fork. The child will terminate immediately (after fork-exec'ing
    * mongocryptd). This orphans mongocryptd, and allows parent to wait on
    * child. */
   pid = fork ();
   if (pid < 0) {
      bson_set_error (error,
                      MONGOC_ERROR_CLIENT,
                      MONGOC_ERROR_CLIENT_INVALID_ENCRYPTION_STATE,
                      "failed to fork (errno=%d) '%s'",
                      errno,
                      strerror (errno));
      return false;
   } else if (pid > 0) {
      int child_status;

      /* Child will spawn mongocryptd and immediately terminate to turn
       * mongocryptd into an orphan. */
      if (waitpid (pid, &child_status, 0 /* options */) < 0) {
         bson_set_error (error,
                         MONGOC_ERROR_CLIENT,
                         MONGOC_ERROR_CLIENT_INVALID_ENCRYPTION_STATE,
                         "failed to wait for child (errno=%d) '%s'",
                         errno,
                         strerror (errno));
         return false;
      }
      /* parent is done at this point, return. */
      return true;
   }

   /* We're no longer in the parent process. Errors encountered result in an
    * exit.
    * Note, we're not logging here, because that would require the user's log
    * callback to be fork-safe.
    */

   /* Start a new session for the child, so it is not bound to the current
    * session (e.g. terminal session). */
   if (setsid () < 0) {
      exit (EXIT_FAILURE);
   }

   /* Fork again. Child terminates so mongocryptd gets orphaned and immedately
    * adopted by init. */
   signal (SIGHUP, SIG_IGN);
   pid = fork ();
   if (pid < 0) {
      exit (EXIT_FAILURE);
   } else if (pid > 0) {
      /* Child terminates immediately. */
      exit (EXIT_SUCCESS);
   }

   /* TODO: Depending on the outcome of MONGOCRYPT-115, possibly change the
    * process's working directory with chdir like: `chdir (default_pid_path)`.
    * Currently pid file ends up in application's working directory. */

   /* Set the user file creation mask to zero. */
   umask (0);

   /* Close and reopen stdin. */
   fd = open ("/dev/null", O_RDONLY);
   if (fd < 0) {
      exit (EXIT_FAILURE);
   }
   dup2 (fd, STDIN_FILENO);
   close (fd);

   /* Close and reopen stdout. */
   fd = open ("/dev/null", O_WRONLY);
   if (fd < 0) {
      exit (EXIT_FAILURE);
   }
   if (dup2 (fd, STDOUT_FILENO) < 0 || close (fd) < 0) {
      exit (EXIT_FAILURE);
   }

   /* Close and reopen stderr. */
   fd = open ("/dev/null", O_RDWR);
   if (fd < 0) {
      exit (EXIT_FAILURE);
   }
   if (dup2 (fd, STDERR_FILENO) < 0 || close (fd) < 0) {
      exit (EXIT_FAILURE);
   }
   fd = 0;

   if (path) {
      to_exec = bson_strdup_printf ("%s%s", path, args[0]);
   } else {
      to_exec = bson_strdup (args[0]);
   }
   if (execvp (to_exec, args) < 0) {
      /* Need to exit. */
      exit (EXIT_FAILURE);
   }

   /* Will never execute. */
   return false;
}
#endif

/*--------------------------------------------------------------------------
 *
 * _mongoc_fle_spawn_mongocryptd --
 *
 *   Attempt to spawn mongocryptd as a background process.
 *
 * Return:
 *   False if an error definitely occurred. Returns true if no reportable
 *   error occurred (though an error may have occurred in starting
 *   mongocryptd, resulting in the process not running).
 *
 * Arguments:
 *    mongocryptd_spawn_path May be NULL, otherwise the path to mongocryptd.
 *    mongocryptd_spawn_args May be NULL, otherwise a bson_iter_t to the
 *    value "mongocryptdSpawnArgs" in AutoEncryptionOpts.extraOptions
 *    (see spec).
 *
 * Post-conditions:
 *    If return false, @error is set.
 *
 *--------------------------------------------------------------------------
 */
bool
_mongoc_fle_spawn_mongocryptd (const char *mongocryptd_spawn_path,
                               const bson_iter_t *mongocryptd_spawn_args,
                               bson_error_t *error)
{
   char **args = NULL;
   bson_iter_t iter;
   bool passed_idle_shutdown_timeout_secs = false;
   int num_args = 2; /* for leading "mongocrypt" and trailing NULL */
   int i;
   bool ret;

   /* iterate once to get length and validate all are strings */
   if (mongocryptd_spawn_args) {
      BSON_ASSERT (BSON_ITER_HOLDS_ARRAY (mongocryptd_spawn_args));
      bson_iter_recurse (mongocryptd_spawn_args, &iter);
      while (bson_iter_next (&iter)) {
         if (!BSON_ITER_HOLDS_UTF8 (&iter)) {
            bson_set_error (error,
                            MONGOC_ERROR_CLIENT,
                            MONGOC_ERROR_CLIENT_INVALID_ENCRYPTION_ARG,
                            "invalid argument for mongocryptd, must be string");
            return false;
         }
         /* Check if the arg starts with --idleShutdownTimeoutSecs= or is equal
          * to --idleShutdownTimeoutSecs */
         if (0 == strncmp ("--idleShutdownTimeoutSecs=",
                           bson_iter_utf8 (&iter, NULL),
                           26) ||
             0 == strcmp ("--idleShutdownTimeoutSecs",
                          bson_iter_utf8 (&iter, NULL))) {
            passed_idle_shutdown_timeout_secs = true;
         }
         num_args++;
      }
   }

   if (!passed_idle_shutdown_timeout_secs) {
      /* add one more */
      num_args++;
   }

   args = (char **) bson_malloc (sizeof (char *) * num_args);
   i = 0;
   args[i++] = "mongocryptd";

   if (mongocryptd_spawn_args) {
      BSON_ASSERT (BSON_ITER_HOLDS_ARRAY (mongocryptd_spawn_args));
      bson_iter_recurse (mongocryptd_spawn_args, &iter);
      while (bson_iter_next (&iter)) {
         args[i++] = (char *) bson_iter_utf8 (&iter, NULL);
      }
   }

   if (!passed_idle_shutdown_timeout_secs) {
      args[i++] = "--idleShutdownTimeoutSecs=60";
   }

   BSON_ASSERT (i == num_args - 1);
   args[i++] = NULL;

   ret = _do_spawn (mongocryptd_spawn_path, args, error);
   bson_free (args);
   return ret;
}

struct _mongoc_client_encryption_opts_t {
   mongoc_client_t *key_vault_client;
   char *key_vault_db;
   char *key_vault_coll;
   bson_t *kms_providers;
};

struct _mongoc_client_encryption_t {
   mongocrypt_t *crypt;
   mongoc_client_t *key_vault_client;
   char *key_vault_db;
   char *key_vault_coll;
   bson_t *kms_providers;
};

struct _mongoc_client_encryption_encrypt_opts_t {
   bson_value_t keyid;
   char *algorithm;
   char *keyaltname;
};

struct _mongoc_client_encryption_datakey_opts_t {
   bson_t *masterkey;
   char **keyaltnames;
   uint32_t keyaltnames_count;
};

mongoc_client_encryption_opts_t*
mongoc_client_encryption_opts_new (void) {
   return bson_malloc0 (sizeof (mongoc_client_encryption_opts_t));
}

void
mongoc_client_encryption_opts_destroy (mongoc_client_encryption_opts_t* opts) {
   bson_free (opts->key_vault_db);
   bson_free (opts->key_vault_coll);
   bson_destroy (opts->kms_providers);
   bson_free (opts);
}

void
mongoc_client_encryption_opts_set_key_vault_client (mongoc_client_encryption_opts_t* opts, mongoc_client_t *key_vault_client) {
   opts->key_vault_client = key_vault_client;
}

void
mongoc_client_encryption_opts_set_key_vault_namespace (mongoc_client_encryption_opts_t *opts, const char* key_vault_db, const char* key_vault_coll) {
   bson_free (opts->key_vault_db);
   opts->key_vault_db = bson_strdup (key_vault_db);
   bson_free (opts->key_vault_coll);
   opts->key_vault_coll = bson_strdup (key_vault_coll);
}

void
mongoc_client_encryption_opts_set_kms_providers (mongoc_client_encryption_opts_t *opts, const bson_t *kms_providers) {
   bson_destroy (opts->kms_providers);
   opts->kms_providers = NULL;
   if (kms_providers) {
      opts->kms_providers = bson_copy (kms_providers);
   }
}

mongoc_client_encryption_t *
mongoc_client_encryption_new (mongoc_client_encryption_opts_t* opts, bson_error_t *error) {
   mongoc_client_encryption_t *client_encryption = NULL;
   _auto_encrypt_init_t *auto_encrypt_init = NULL;
   bool success = false;

   /* Check for required options */
   if (!opts->key_vault_db || !opts->key_vault_coll) {
      bson_set_error (error,
                      MONGOC_ERROR_CLIENT,
                      MONGOC_ERROR_CLIENT_INVALID_ENCRYPTION_ARG,
                      "Key vault namespace option required");
      goto fail;
   }

   if (!opts->kms_providers) {
      bson_set_error (error,
                      MONGOC_ERROR_CLIENT,
                      MONGOC_ERROR_CLIENT_INVALID_ENCRYPTION_ARG,
                      "KMS providers option required");
      goto fail;
   }


   client_encryption = bson_malloc0 (sizeof (*client_encryption));
   client_encryption->key_vault_client = opts->key_vault_client;
   client_encryption->key_vault_db = bson_strdup (opts->key_vault_db);
   client_encryption->key_vault_coll = bson_strdup (opts->key_vault_coll);
   client_encryption->kms_providers = bson_copy (opts->kms_providers);
   client_encryption->crypt = _create_mongocrypt (opts->kms_providers, NULL /* schema_map */, error);
   if (!client_encryption->crypt) {
      goto fail;
   }
   success = true;

fail:
   _auto_encrypt_init_destroy (auto_encrypt_init);
   if (!success) {
      mongoc_client_encryption_destroy (client_encryption);
      return NULL;
   }
   return client_encryption;
}

void
mongoc_client_encryption_destroy (mongoc_client_encryption_t *client_encryption) {
   mongocrypt_destroy (client_encryption->crypt);
   bson_free (client_encryption->key_vault_db);
   bson_free (client_encryption->key_vault_coll);
   bson_destroy (client_encryption->kms_providers);
   bson_free (client_encryption);
}

bool
mongoc_client_encryption_create_data_key (mongoc_client_encryption_t* client_encryption, const char* kms_provider, mongoc_client_encryption_datakey_opts_t* opts, bson_value_t *keyid, bson_error_t* error) {
   mongocrypt_ctx_t *ctx = NULL;
   bool ret = false;
   _auto_encrypt_t* auto_encrypt = NULL;
   bson_t* datakey = NULL;
   mongoc_write_concern_t *wc = NULL;
   bson_t insert_opts = BSON_INITIALIZER;
   mongoc_collection_t *coll = NULL;

   ENTRY;

   /* reset, so it is safe for caller to call bson_value_destroy on error or success. */
   if (keyid) {
      keyid->value_type = BSON_TYPE_EOD;
   }

   auto_encrypt = _auto_encrypt_new (client_encryption->crypt, NULL /* mongocryptd_client */, client_encryption->key_vault_client, NULL /* collinfo_client */, client_encryption->key_vault_db, client_encryption->key_vault_coll, false /* bypass_auto_encryption */);

   /* Create the context for the operation. */
   ctx = mongocrypt_ctx_new (auto_encrypt->crypt);
   if (!ctx) {
      _crypt_check_error (auto_encrypt->crypt, error, true);
      goto fail;
   }

   if (0 == strcmp ("aws", kms_provider) && opts->masterkey) {
      bson_iter_t iter;
      const char* region = NULL;
      uint32_t region_len = 0;
      const char* key = NULL;
      uint32_t key_len = 0;

      if (bson_iter_init_find (&iter, opts->masterkey, "region") && BSON_ITER_HOLDS_UTF8 (&iter)) {
         region = bson_iter_utf8 (&iter, &region_len);
      }

      if (bson_iter_init_find (&iter, opts->masterkey, "key") && BSON_ITER_HOLDS_UTF8 (&iter)) {
         key = bson_iter_utf8 (&iter, &key_len);
      }

      if (!mongocrypt_ctx_setopt_masterkey_aws (ctx, region, region_len, key, key_len)) {
         _ctx_check_error (ctx, error, true);
         goto fail;
      }

      /* Check for optional endpoint */
      if (bson_iter_init_find (&iter, opts->masterkey, "endpoint") && BSON_ITER_HOLDS_UTF8 (&iter)) {
         const char* endpoint = NULL;
         uint32_t endpoint_len = 0;

         endpoint = bson_iter_utf8 (&iter, &endpoint_len);
         if (!mongocrypt_ctx_setopt_masterkey_aws_endpoint (ctx, endpoint, endpoint_len)) {
            _ctx_check_error (ctx, error, true);
            goto fail;
         }
      }

   }
   
   if (0 == strcmp ("local", kms_provider)) {
      if (!mongocrypt_ctx_setopt_masterkey_local (ctx)) {
         _ctx_check_error (ctx, error, true);
         goto fail;
      }
   }
   

   if (opts->keyaltnames) {
      int i;

      for (i = 0; i < opts->keyaltnames_count; i++) {
         bool keyaltname_ret;
         mongocrypt_binary_t *keyaltname_bin;
         bson_t *keyaltname_doc;

         keyaltname_doc = BCON_NEW ("keyAltName", opts->keyaltnames[i]);
         keyaltname_bin = mongocrypt_binary_new_from_data ((uint8_t*)bson_get_data (keyaltname_doc), keyaltname_doc->len);
         keyaltname_ret = mongocrypt_ctx_setopt_key_alt_name (ctx, keyaltname_bin);
         mongocrypt_binary_destroy (keyaltname_bin);
         bson_destroy (keyaltname_doc);
         if (!keyaltname_ret) {
             _ctx_check_error (ctx, error, true);
            goto fail;
         }
      }
   }

   if (!mongocrypt_ctx_datakey_init (ctx)) {
      _ctx_check_error (ctx, error, true);
      goto fail;
   }

   if (!_mongoc_cse_run_state_machine (
          auto_encrypt, NULL /* db_name */, ctx, &datakey, error)) {
      goto fail;
   }

   if (!datakey) {
      bson_set_error (error, MONGOC_ERROR_CLIENT, MONGOC_ERROR_CLIENT_INVALID_ENCRYPTION_STATE, "data key not created due to unknown error");
      goto fail;
   }

   /* Insert the data key with write concern majority */
   wc = mongoc_write_concern_new ();
   mongoc_write_concern_set_wmajority (wc, 1000);
   coll = mongoc_client_get_collection (client_encryption->key_vault_client, client_encryption->key_vault_db, client_encryption->key_vault_coll);
   mongoc_collection_set_write_concern (coll, wc);
   ret = mongoc_collection_insert_one (coll, datakey, NULL /* opts */, NULL /* reply */, error);
   if (!ret) {
      goto fail;
   }

   if (keyid) {
      bson_iter_t iter;
      const bson_value_t * id_value;

      if (!bson_iter_init_find (&iter, datakey, "_id")) {
         bson_set_error (error, MONGOC_ERROR_CLIENT, MONGOC_ERROR_CLIENT_INVALID_ENCRYPTION_STATE, "data key not did not contain _id");
         goto fail;
      } else {
         id_value = bson_iter_value (&iter);
         bson_value_copy (id_value, keyid);
      }
   }

   ret = true;

fail:
   bson_destroy (&insert_opts);
   bson_destroy (datakey);
   mongocrypt_ctx_destroy (ctx);
   _auto_encrypt_destroy (auto_encrypt);
   mongoc_write_concern_destroy (wc);
   mongoc_collection_destroy (coll);
   RETURN (ret);
}

bool
mongoc_client_encryption_encrypt (mongoc_client_encryption_t* client_encryption, bson_value_t *value, mongoc_client_encryption_encrypt_opts_t* opts, bson_value_t* ciphertext, bson_error_t* error) {
   mongocrypt_ctx_t *ctx = NULL;
   bool ret = false;
   _auto_encrypt_t* auto_encrypt = NULL;
   bson_t* to_encrypt_doc = NULL;
   mongocrypt_binary_t *to_encrypt_bin = NULL;
   bson_t* result = NULL;
   bson_iter_t iter;

   ENTRY;

   /* reset, so it is safe for caller to call bson_value_destroy on error or success. */
   if (ciphertext) {
      ciphertext->value_type = BSON_TYPE_EOD;
   }

   auto_encrypt = _auto_encrypt_new (client_encryption->crypt, NULL /* mongocryptd_client */, client_encryption->key_vault_client, NULL /* collinfo_client */, client_encryption->key_vault_db, client_encryption->key_vault_coll, false /* bypass_auto_encryption */);

   /* Create the context for the operation. */
   ctx = mongocrypt_ctx_new (auto_encrypt->crypt);
   if (!ctx) {
      _crypt_check_error (auto_encrypt->crypt, error, true);
      goto fail;
   }

   if (!mongocrypt_ctx_setopt_algorithm (ctx, opts->algorithm, -1)) {
      _ctx_check_error (ctx, error, true);
      goto fail;
   }

   if (opts->keyaltname) {
      bool keyaltname_ret;
      mongocrypt_binary_t *keyaltname_bin;
      bson_t *keyaltname_doc;

      keyaltname_doc = BCON_NEW ("keyAltName", opts->keyaltname);
      keyaltname_bin = mongocrypt_binary_new_from_data ((uint8_t*)bson_get_data (keyaltname_doc), keyaltname_doc->len);
      keyaltname_ret = mongocrypt_ctx_setopt_key_alt_name (ctx, keyaltname_bin);
      mongocrypt_binary_destroy (keyaltname_bin);
      bson_destroy (keyaltname_doc);
      if (!keyaltname_ret) {
            _ctx_check_error (ctx, error, true);
         goto fail;
      }
   }

   if (opts->keyid.value_type == BSON_TYPE_BINARY) {
      mongocrypt_binary_t *keyid_bin;
      bool keyid_ret;

      if (opts->keyid.value.v_binary.subtype != BSON_SUBTYPE_UUID) {
         bson_set_error (error, MONGOC_ERROR_CLIENT, MONGOC_ERROR_CLIENT_INVALID_ENCRYPTION_ARG, "keyid must be a UUID");
         goto fail;
      }

      keyid_bin = mongocrypt_binary_new_from_data (opts->keyid.value.v_binary.data, opts->keyid.value.v_binary.data_len);
      keyid_ret = mongocrypt_ctx_setopt_key_id (ctx, keyid_bin);
      mongocrypt_binary_destroy (keyid_bin);
      if (!keyid_ret) {
         _ctx_check_error (ctx, error, true);
         goto fail;
      }
   }

   to_encrypt_doc = bson_new ();
   BSON_APPEND_VALUE (to_encrypt_doc, "v", value);
   to_encrypt_bin = mongocrypt_binary_new_from_data ((uint8_t*)bson_get_data (to_encrypt_doc), to_encrypt_doc->len);
   if (!mongocrypt_ctx_explicit_encrypt_init (ctx, to_encrypt_bin)) {
      _ctx_check_error (ctx, error, true);
      goto fail;
   }

   if (!_mongoc_cse_run_state_machine (
          auto_encrypt, NULL /* db_name */, ctx, &result, error)) {
      goto fail;
   }

   if (!result) {
      bson_set_error (error, MONGOC_ERROR_CLIENT, MONGOC_ERROR_CLIENT_INVALID_ENCRYPTION_STATE, "could not encrypt due to unknown error");
      goto fail;
   }

   /* extract value */
   if (!bson_iter_init_find (&iter, result, "v")) {
      bson_set_error (error, MONGOC_ERROR_CLIENT, MONGOC_ERROR_CLIENT_INVALID_ENCRYPTION_STATE, "encrypted result unexpected");
      goto fail;
   } else {
      const bson_value_t* tmp;

      tmp = bson_iter_value (&iter);
      bson_value_copy (tmp, ciphertext);
   }

   ret = true;
fail:
   bson_destroy (to_encrypt_doc);
   mongocrypt_binary_destroy (to_encrypt_bin);
   bson_destroy (result);
   mongocrypt_ctx_destroy (ctx);
   _auto_encrypt_destroy (auto_encrypt);
   RETURN (ret);
}

bool
mongoc_client_encryption_decrypt (mongoc_client_encryption_t* client_encryption, bson_value_t* ciphertext, bson_value_t *value, bson_error_t* error) {
   mongocrypt_ctx_t *ctx = NULL;
   bool ret = false;
   _auto_encrypt_t* auto_encrypt = NULL;
   bson_t* to_decrypt_doc = NULL;
   mongocrypt_binary_t *to_decrypt_bin = NULL;
   bson_t* result = NULL;
   bson_iter_t iter;

   ENTRY;

   /* reset, so it is safe for caller to call bson_value_destroy on error or success. */
   if (value) {
      value->value_type = BSON_TYPE_EOD;
   }

   auto_encrypt = _auto_encrypt_new (client_encryption->crypt, NULL /* mongocryptd_client */, client_encryption->key_vault_client, NULL /* collinfo_client */, client_encryption->key_vault_db, client_encryption->key_vault_coll, false /* bypass_auto_encryption */);

   /* Create the context for the operation. */
   ctx = mongocrypt_ctx_new (auto_encrypt->crypt);
   if (!ctx) {
      _crypt_check_error (auto_encrypt->crypt, error, true);
      goto fail;
   }

   to_decrypt_doc = bson_new ();
   BSON_APPEND_VALUE (to_decrypt_doc, "v", ciphertext);
   to_decrypt_bin = mongocrypt_binary_new_from_data ((uint8_t*)bson_get_data (to_decrypt_doc), to_decrypt_doc->len);
   if (!mongocrypt_ctx_explicit_decrypt_init (ctx, to_decrypt_bin)) {
      _ctx_check_error (ctx, error, true);
      goto fail;
   }

   if (!_mongoc_cse_run_state_machine (
          auto_encrypt, NULL /* db_name */, ctx, &result, error)) {
      goto fail;
   }

   if (!result) {
      bson_set_error (error, MONGOC_ERROR_CLIENT, MONGOC_ERROR_CLIENT_INVALID_ENCRYPTION_STATE, "could not decrypt due to unknown error");
      goto fail;
   }

   /* extract value */
   if (!bson_iter_init_find (&iter, result, "v")) {
      bson_set_error (error, MONGOC_ERROR_CLIENT, MONGOC_ERROR_CLIENT_INVALID_ENCRYPTION_STATE, "decrypted result unexpected");
      goto fail;
   } else {
      const bson_value_t* tmp;

      tmp = bson_iter_value (&iter);
      bson_value_copy (tmp, value);
   }

   ret = true;
fail:
   bson_destroy (to_decrypt_doc);
   mongocrypt_binary_destroy (to_decrypt_bin);
   bson_destroy (result);
   mongocrypt_ctx_destroy (ctx);
   _auto_encrypt_destroy (auto_encrypt);
   RETURN (ret);
}

mongoc_client_encryption_encrypt_opts_t*
mongoc_client_encryption_encrypt_opts_new (void) {
   return bson_malloc0 (sizeof (mongoc_client_encryption_encrypt_opts_t));
}

void
mongoc_client_encryption_encrypt_opts_destroy (mongoc_client_encryption_encrypt_opts_t* opts) {
   if (!opts) {
      return;
   }
   bson_value_destroy (&opts->keyid);
   bson_free (opts->algorithm);
   bson_free (opts->keyaltname);
   bson_free (opts);
}

void
mongoc_client_encryption_encrypt_opts_set_keyid (mongoc_client_encryption_encrypt_opts_t* opts, const bson_value_t* keyid) {
   bson_value_destroy (&opts->keyid);
   memset (&opts->keyid, 0, sizeof (opts->keyid));
   if (keyid) {
      bson_value_copy (keyid, &opts->keyid);
   }
}

void
mongoc_client_encryption_encrypt_opts_set_keyaltname (mongoc_client_encryption_encrypt_opts_t* opts, const char* keyaltname) {
   bson_free (opts->keyaltname);
   opts->keyaltname = NULL;
   if (keyaltname) {
      opts->keyaltname = bson_strdup(keyaltname);
   }
}

void
mongoc_client_encryption_encrypt_opts_set_algorithm (mongoc_client_encryption_encrypt_opts_t* opts, const char* algorithm) {
   bson_free (opts->algorithm);
   opts->algorithm = NULL;
   if (algorithm) {
      opts->algorithm = bson_strdup(algorithm);
   }
}

mongoc_client_encryption_datakey_opts_t*
mongoc_client_encryption_datakey_opts_new () {
   return bson_malloc0 (sizeof (mongoc_client_encryption_datakey_opts_t));
}

void
mongoc_client_encryption_datakey_opts_destroy (mongoc_client_encryption_datakey_opts_t* opts) {
   if (!opts) {
      return;
   }

   bson_destroy (opts->masterkey);
   if (opts->keyaltnames) {
      int i;
      
      for (i = 0; i < opts->keyaltnames_count; i++) {
         bson_free (opts->keyaltnames[i]);
      }
      bson_free (opts->keyaltnames);
      opts->keyaltnames_count = 0;
   }

   bson_free (opts);
}

void
mongoc_client_encryption_datakey_opts_set_masterkey (mongoc_client_encryption_datakey_opts_t *opts, const bson_t* masterkey) {
   bson_destroy (opts->masterkey);
   opts->masterkey = NULL;
   if (masterkey) {
      opts->masterkey = bson_copy (masterkey);
   }
}

void
mongoc_client_encryption_datakey_opts_set_keyaltnames (mongoc_client_encryption_datakey_opts_t *opts, char ** keyaltnames, uint32_t keyaltnames_count) {
   int i;

   if (opts->keyaltnames_count) {
      for (i = 0; i < opts->keyaltnames_count; i++) {
         bson_free (opts->keyaltnames[i]);
      }
      bson_free (opts->keyaltnames);
      opts->keyaltnames = NULL;
      opts->keyaltnames_count = 0;
   }

   if (keyaltnames_count) {
      opts->keyaltnames = bson_malloc (sizeof(char*) * keyaltnames_count);
      for (i = 0; i < keyaltnames_count; i++) {
         opts->keyaltnames[i] = bson_strdup (keyaltnames[i]);
      }
      opts->keyaltnames_count = keyaltnames_count;
   }
}

#endif /* MONGOC_ENABLE_CLIENT_SIDE_ENCRYPTION */