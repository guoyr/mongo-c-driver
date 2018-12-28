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

#include "mongoc/mongoc.h"
#include "mongoc/mongoc-crypt-private.h"
/* careful, don't rely on private headers. libmongocrypt should depend on
 * libmongoc, not reach inside. This is okay for now, we're only using this
 * for the client methods that would otherwise be in mongoc-client.c */
#include "mongoc/mongoc-client-private.h"


const char *
tmp_json (const bson_t *bson)
{
   static char storage[1024];
   char *json;

   memset (storage, 0, 1024);
   json = bson_as_json (bson, NULL);
   bson_snprintf (storage, sizeof (storage), "%s", json);
   bson_free (json);
   return (const char *) storage;
}

static void
_spawn_mongocryptd (void)
{
/* oddly, starting mongocryptd in libmongoc starts multiple instances. */
#ifdef SPAWN_BUG_FIXED
   pid_t pid = fork ();

   CRYPT_ENTRY;
   CRYPT_TRACE ("spawning mongocryptd\n");
   if (pid == 0) {
      int ret;
      /* child */
      CRYPT_TRACE ("child starting mongocryptd\n");
      ret = execlp ("mongocryptd", "mongocryptd", (char *) NULL);
      if (ret == -1) {
         MONGOC_ERROR ("child process unable to exec mongocryptd");
         abort ();
      }
   }
#endif
}


mongoc_crypt_t *
_mongoc_crypt_new (mongoc_client_t *client, const bson_t* opts, bson_error_t *error)
{
   /* store AWS credentials, init structures in client, store schema
    * somewhere. */
   mongoc_crypt_t *crypt;

   CRYPT_ENTRY;
   _spawn_mongocryptd ();
   crypt = bson_malloc0 (sizeof (mongoc_crypt_t));
   if (!_mongoc_client_side_encryption_opts_parse (
      NULL, opts, &crypt->opts, error)) {
      _mongoc_client_side_encryption_opts_cleanup (&crypt->opts);
      bson_free (crypt);
      return NULL;
   }

   crypt->mongocryptd_client =
      mongoc_client_new ("mongodb://%2Ftmp%2Fmongocryptd.sock");
   if (!crypt->mongocryptd_client) {
      SET_CRYPT_ERR ("Unable to create client to mongocryptd");
      return crypt;
   }
   /* TODO: use 'u' from schema to get key vault clients. Note no opts here. */
   crypt->keyvault_client =
      mongoc_client_new_from_uri (mongoc_client_get_uri (client));
   if (!crypt->keyvault_client) {
      SET_CRYPT_ERR ("Unable to create client to keyvault");
      return crypt;
   }
   return crypt;
}


void
_mongoc_crypt_destroy (mongoc_crypt_t *crypt)
{
   CRYPT_ENTRY;
   if (!crypt) {
      return;
   }
   _mongoc_client_side_encryption_opts_cleanup (&crypt->opts);
   mongoc_client_destroy (crypt->mongocryptd_client);
   mongoc_client_destroy (crypt->keyvault_client);
   bson_free (crypt);
}


/*
 * _get_key
*/
static bool
_get_key (mongoc_crypt_t *crypt,
          mongoc_crypt_binary_t *key_id,
          const char *key_alt_name,
          mongoc_crypt_key_t *out,
          bson_error_t *error)
{
   mongoc_collection_t *datakey_coll;
   mongoc_cursor_t *cursor;
   bson_t filter;
   const bson_t *doc;
   bool ret = false;

   CRYPT_ENTRY;
   datakey_coll = mongoc_client_get_collection (
      crypt->keyvault_client, "admin", "datakeys");
   bson_init (&filter);
   if (key_id->len) {
      mongoc_crypt_bson_append_binary (&filter, "_id", 3, key_id);
   } else if (key_alt_name) {
      bson_append_utf8 (
         &filter, "keyAltName", 10, key_alt_name, (int) strlen (key_alt_name));
   } else {
      SET_CRYPT_ERR ("must provide key id or alt name");
      bson_destroy (&filter);
      return ret;
   }

   CRYPT_TRACE ("finding key by filter: %s", tmp_json (&filter));
   cursor =
      mongoc_collection_find_with_opts (datakey_coll, &filter, NULL, NULL);
   bson_destroy (&filter);

   if (!mongoc_cursor_next (cursor, &doc)) {
      SET_CRYPT_ERR ("key not found");
      goto cleanup;
   }

   CRYPT_TRACE ("got key: %s\n", tmp_json (doc));
   if (!_mongoc_crypt_key_parse (doc, out, error)) {
      goto cleanup;
   }

   CRYPT_TRACE("decrypting key_material");
   if (!_mongoc_crypt_kms_decrypt (crypt, out, error)) {
      goto cleanup;
   }

   ret = true;

cleanup:
   mongoc_cursor_destroy (cursor);
   mongoc_collection_destroy (datakey_coll);
   return ret;
}

static bool
_get_key_by_uuid (mongoc_crypt_t *crypt,
                  mongoc_crypt_binary_t *key_id,
                  mongoc_crypt_key_t *out,
                  bson_error_t *error)
{
   CRYPT_ENTRY;
   return _get_key (crypt, key_id, NULL, out, error);
}


static bool
_append_encrypted (mongoc_crypt_t *crypt,
                   mongoc_crypt_marking_t *marking,
                   bson_t *out,
                   const char *field,
                   uint32_t field_len,
                   bson_error_t *error)
{
   bool ret = false;
   /* will hold { 'k': <key id>, 'iv': <iv>, 'e': <encrypted data> } */
   bson_t encrypted_w_metadata = BSON_INITIALIZER;
   /* will hold { 'e': <encrypted data> } */
   bson_t to_encrypt = BSON_INITIALIZER;
   uint8_t *encrypted = NULL;
   uint32_t encrypted_len;
   mongoc_crypt_key_t key = {{0}};

   CRYPT_ENTRY;
   if (!_get_key (
          crypt, &marking->key_id, marking->key_alt_name, &key, error)) {
      SET_CRYPT_ERR ("could not get key");
      goto cleanup;
   }

   bson_append_iter (&to_encrypt, "v", 1, &marking->v_iter);
   /* TODO: 'a' and 'u' */

   if (!_mongoc_crypt_do_encryption (marking->iv.data,
                                     key.data_key.data,
                                     bson_get_data (&to_encrypt),
                                     to_encrypt.len,
                                     &encrypted,
                                     &encrypted_len,
                                     error)) {
      goto cleanup;
   }

   CRYPT_TRACE ("did encryption");

   /* append { 'k': <key id>, 'iv': <iv>, 'e': <encrypted { v: <val> } > } */
   mongoc_crypt_bson_append_binary (
      &encrypted_w_metadata, "k", 1, &marking->key_id);
   mongoc_crypt_bson_append_binary (
      &encrypted_w_metadata, "iv", 2, &marking->iv);
   bson_append_binary (&encrypted_w_metadata,
                       "e",
                       1,
                       BSON_SUBTYPE_BINARY,
                       encrypted,
                       encrypted_len);
   bson_append_binary (out,
                       field,
                       field_len,
                       BSON_SUBTYPE_ENCRYPTED,
                       bson_get_data (&encrypted_w_metadata),
                       encrypted_w_metadata.len);

   ret = true;

cleanup:
   bson_destroy (&to_encrypt);
   bson_free (encrypted);
   bson_destroy (&encrypted_w_metadata);
   mongoc_crypt_key_cleanup(&key);
   return ret;
}


static bool
_append_decrypted (mongoc_crypt_t *crypt,
                   mongoc_crypt_encrypted_t *encrypted,
                   bson_t *out,
                   const char *field,
                   uint32_t field_len,
                   bson_error_t *error)
{
   mongoc_crypt_key_t key = {{0}};
   uint8_t *decrypted;
   uint32_t decrypted_len;
   bool ret = false;

   CRYPT_ENTRY;
   if (!_get_key_by_uuid (crypt, &encrypted->key_id, &key, error)) {
      return ret;
   }

   if (!_mongoc_crypt_do_decryption (encrypted->iv.data,
                                     key.data_key.data,
                                     encrypted->e.data,
                                     encrypted->e.len,
                                     &decrypted,
                                     &decrypted_len,
                                     error)) {
      goto cleanup;
   } else {
      bson_t wrapped; /* { 'v': <the value> } */
      bson_iter_t wrapped_iter;
      bson_init_static (&wrapped, decrypted, decrypted_len);
      if (!bson_iter_init_find (&wrapped_iter, &wrapped, "v")) {
         bson_destroy (&wrapped);
         SET_CRYPT_ERR ("invalid encrypted data, missing 'v' field");
         goto cleanup;
      }
      bson_append_value (
         out, field, field_len, bson_iter_value (&wrapped_iter));
      bson_destroy (&wrapped);
   }

   ret = true;

cleanup:
   bson_free (decrypted);
   mongoc_crypt_key_cleanup(&key);
   return ret;
}

typedef enum { MARKING_TO_ENCRYPTED, ENCRYPTED_TO_PLAIN } transform_t;

static bool
_copy_and_transform (mongoc_crypt_t *crypt,
                     bson_iter_t iter,
                     bson_t *out,
                     bson_error_t *error,
                     transform_t transform)
{
   CRYPT_ENTRY;
   while (bson_iter_next (&iter)) {
      if (BSON_ITER_HOLDS_BINARY (&iter)) {
         mongoc_crypt_binary_t value;
         bson_t as_bson;

         mongoc_crypt_binary_from_iter_unowned (&iter, &value);
         bson_init_static (&as_bson, value.data, value.len);
         CRYPT_TRACE ("found FLE binary: %s", tmp_json (&as_bson));
         if (value.subtype == BSON_SUBTYPE_ENCRYPTED) {
            if (transform == MARKING_TO_ENCRYPTED) {
               mongoc_crypt_marking_t marking = {{0}};

               if (!_mongoc_crypt_marking_parse_unowned (&as_bson, &marking, error)) {
                  return false;
               }
               if (!_append_encrypted (crypt,
                                       &marking,
                                       out,
                                       bson_iter_key (&iter),
                                       bson_iter_key_len (&iter),
                                       error))
                  return false;
            } else {
               mongoc_crypt_encrypted_t encrypted = {{0}};

               if (!_mongoc_crypt_encrypted_parse_unowned (
                      &as_bson, &encrypted, error)) {
                  return false;
               }
               if (!_append_decrypted (crypt,
                                       &encrypted,
                                       out,
                                       bson_iter_key (&iter),
                                       bson_iter_key_len (&iter),
                                       error))
                  return false;
            }
            continue;
         }
         /* otherwise, fall through. copy over like a normal value. */
      }

      if (BSON_ITER_HOLDS_ARRAY (&iter)) {
         bson_iter_t child_iter;
         bson_t child_out;
         bool ret;

         bson_iter_recurse (&iter, &child_iter);
         bson_append_array_begin (
            out, bson_iter_key (&iter), bson_iter_key_len (&iter), &child_out);
         ret = _copy_and_transform (
            crypt, child_iter, &child_out, error, transform);
         bson_append_array_end (out, &child_out);
         if (!ret) {
            return false;
         }
      } else if (BSON_ITER_HOLDS_DOCUMENT (&iter)) {
         bson_iter_t child_iter;
         bson_t child_out;
         bool ret;

         bson_iter_recurse (&iter, &child_iter);
         bson_append_document_begin (
            out, bson_iter_key (&iter), bson_iter_key_len (&iter), &child_out);
         ret = _copy_and_transform (
            crypt, child_iter, &child_out, error, transform);
         bson_append_document_end (out, &child_out);
         if (!ret) {
            return false;
         }
      } else {
         bson_append_value (out,
                            bson_iter_key (&iter),
                            bson_iter_key_len (&iter),
                            bson_iter_value (&iter));
      }
   }
   return true;
}


static bool
_replace_markings (mongoc_crypt_t *crypt,
                   const bson_t *reply,
                   bson_t *out,
                   bson_error_t *error)
{
   bson_iter_t iter;

   CRYPT_ENTRY;
   BSON_ASSERT (bson_iter_init_find (&iter, reply, "ok"));
   if (!bson_iter_as_bool (&iter)) {
      SET_CRYPT_ERR ("markFields returned ok:0");
      return false;
   }

   if (!bson_iter_init_find (&iter, reply, "data")) {
      SET_CRYPT_ERR ("markFields returned ok:0");
      return false;
   }
   /* recurse into array. */
   bson_iter_recurse (&iter, &iter);
   bson_iter_next (&iter);
   /* recurse into first document. */
   bson_iter_recurse (&iter, &iter);
   if (!_copy_and_transform (crypt, iter, out, error, MARKING_TO_ENCRYPTED)) {
      return false;
   }
   return true;
}


static void
_make_marking_cmd (const bson_t *data, const bson_t *schema, bson_t *cmd)
{
   bson_t child;

   bson_init (cmd);
   BSON_APPEND_INT64 (cmd, "markFields", 1);
   BSON_APPEND_ARRAY_BEGIN (cmd, "data", &child);
   BSON_APPEND_DOCUMENT (&child, "0", data);
   bson_append_array_end (cmd, &child);
   BSON_APPEND_DOCUMENT (cmd, "schema", schema);
}

bool
mongoc_crypt_encrypt (mongoc_crypt_t *crypt,
                      const bson_t *schema,
                      const bson_t *doc,
                      bson_t *out,
                      bson_error_t *error)
{
   bson_t cmd, reply;
   bool ret;

   CRYPT_ENTRY;
   ret = false;
   bson_init (out);
   _make_marking_cmd (doc, schema, &cmd);
   if (!mongoc_client_command_simple (crypt->mongocryptd_client,
                                      "admin",
                                      &cmd,
                                      NULL /* read prefs */,
                                      &reply,
                                      error)) {
      goto cleanup;
   }

   CRYPT_TRACE ("sent marking cmd: %s", tmp_json (&cmd));
   CRYPT_TRACE ("got back: %s", tmp_json (&reply));

   if (!_replace_markings (crypt, &reply, out, error)) {
      goto cleanup;
   }

   ret = true;
cleanup:
   bson_destroy (&cmd);
   bson_destroy (&reply);
   return ret;
}

bool
mongoc_crypt_decrypt (mongoc_crypt_t *crypt,
                      const bson_t *doc,
                      bson_t *out,
                      bson_error_t *error)
{
   bson_iter_t iter;

   CRYPT_ENTRY;
   bson_iter_init (&iter, doc);
   bson_init (out);
   if (!_copy_and_transform (crypt, iter, out, error, ENCRYPTED_TO_PLAIN)) {
      return false;
   }
   return true;
}

/* Functions that go outside of libmongocrypt */

/*
 * Returns false if the collection has no known encrypted fields.
 * Initializes schema regardless.
 */
bool
_mongoc_client_get_schema (mongoc_client_t *client,
                           const char *ns,
                           bson_t *schema)
{
   /* TODO: do remote fetching and use JSONSchema cache. */
   bson_iter_t array_iter;
   bson_iter_init (&array_iter, &client->crypt->opts.schemas);
   const uint8_t *data;
   uint32_t len;

   while (bson_iter_next (&array_iter)) {
      bson_iter_t doc_iter;
      bson_iter_recurse (&array_iter, &doc_iter);
      if (!bson_iter_find (&doc_iter, "ns")) {
         continue;
      }

      if (0 != strcmp (bson_iter_utf8 (&doc_iter, NULL), ns)) {
         continue;
      }

      bson_iter_recurse (&array_iter, &doc_iter);
      if (!bson_iter_find (&doc_iter, "schema")) {
         continue;
      }

      bson_iter_document (&doc_iter, &len, &data);
      bson_init_static (schema, data, len);
      return true;
   }

   bson_init (schema);
   return false;
}


mongoc_client_t *
mongoc_client_new_with_opts (mongoc_uri_t *uri,
                             bson_t *opts,
                             bson_error_t *error)
{
   mongoc_client_t *client;
   bson_iter_t iter;

   client = mongoc_client_new_from_uri (uri);
   if (!client)
      return NULL;


   /* TODO: generate-opts.py only supports top-level options. I can't nest
    * a Struct within a Struct in generate-opts.py and have it validate
    * recursively. Consider changing. */
   if (opts && bson_iter_init_find (&iter, opts, "clientSideEncryption")) {
      const uint8_t *data;
      uint32_t len;
      bson_t nested_opts;

      if (!BSON_ITER_HOLDS_DOCUMENT (&iter)) {
         bson_set_error (error,
                         MONGOC_ERROR_BSON,
                         MONGOC_ERROR_BSON_INVALID,
                         "clientSideEncryption must be a document.");
      }

      bson_iter_document (&iter, &len, &data);
      bson_init_static (&nested_opts, data, len);

      client->crypt = _mongoc_crypt_new (client, &nested_opts, error);
      if (!client->crypt) {
         mongoc_client_destroy (client);
         return NULL;
      }
   }

   return client;
}