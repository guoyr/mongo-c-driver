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

#include <bson/bson.h>

#include "mongoc-error.h"
#include "mongoc-error-private.h"
#include "mongoc-rpc-private.h"

bool
mongoc_error_has_label (const bson_t *reply, const char *label)
{
   bson_iter_t iter;
   bson_iter_t error_labels;

   BSON_ASSERT (reply);
   BSON_ASSERT (label);

   if (bson_iter_init_find (&iter, reply, "errorLabels") &&
       bson_iter_recurse (&iter, &error_labels)) {
      while (bson_iter_next (&error_labels)) {
         if (BSON_ITER_HOLDS_UTF8 (&error_labels) &&
             !strcmp (bson_iter_utf8 (&error_labels, NULL), label)) {
            return true;
         }
      }
   }

   if (!bson_iter_init_find (&iter, reply, "writeConcernError")) {
      return false;
   }

   BSON_ASSERT (bson_iter_recurse (&iter, &iter));

   if (bson_iter_find (&iter, "errorLabels") &&
       bson_iter_recurse (&iter, &error_labels)) {
      while (bson_iter_next (&error_labels)) {
         if (BSON_ITER_HOLDS_UTF8 (&error_labels) &&
             !strcmp (bson_iter_utf8 (&error_labels, NULL), label)) {
            return true;
         }
      }
   }

   return false;
}

static bool
_mongoc_write_error_is_retryable (bson_error_t *error)
{
   switch (error->code) {
   case 6:     /* HostUnreachable */
   case 7:     /* HostNotFound */
   case 89:    /* NetworkTimeout */
   case 91:    /* ShutdownInProgress */
   case 189:   /* PrimarySteppedDown */
   case 262:   /* ExceededTimeLimit */
   case 9001:  /* SocketException */
   case 10107: /* NotMaster */
   case 11600: /* InterruptedAtShutdown */
   case 11602: /* InterruptedDueToReplStateChange */
   case 13435: /* NotMasterNoSlaveOk */
   case 13436: /* NotMasterOrSecondary */
      return true;
   default:
      if (strstr (error->message, "not master") ||
          strstr (error->message, "node is recovering")) {
         return true;
      }
      return false;
   }
}

static void
_mongoc_write_error_append_retryable_label (bson_t *reply)
{
   bson_t reply_local = BSON_INITIALIZER;

   if (!reply) {
      bson_destroy (&reply_local);
      return;
   }

   bson_copy_to_excluding_noinit (reply, &reply_local, "errorLabels", NULL);
   _mongoc_error_copy_labels_and_upsert (
      reply, &reply_local, RETRYABLE_WRITE_ERROR);

   bson_destroy (reply);
   bson_steal (reply, &reply_local);
}

void
_mongoc_write_error_handle_labels (bool cmd_ret,
                                   const bson_error_t *cmd_err,
                                   bson_t *reply,
                                   bool supports_retryable_write_label)
{
   bson_error_t error;

   /* check for a client error. */
   if (!cmd_ret && (cmd_err->domain == MONGOC_ERROR_STREAM ||
                    (cmd_err->domain == MONGOC_ERROR_PROTOCOL &&
                     cmd_err->code == MONGOC_ERROR_PROTOCOL_INVALID_REPLY))) {
      /* Retryable writes spec: When the driver encounters a network error
       * communicating with any server version that supports retryable
       * writes, it MUST add a RetryableWriteError label to that error. */
      _mongoc_write_error_append_retryable_label (reply);
      return;
   }

   if (supports_retryable_write_label) {
      return;
   }

   /* check for a server error. */
   if (_mongoc_cmd_check_ok_no_wce (
          reply, MONGOC_ERROR_API_VERSION_2, &error)) {
      return;
   }

   if (_mongoc_write_error_is_retryable (&error)) {
      _mongoc_write_error_append_retryable_label (reply);
   }
}


/*--------------------------------------------------------------------------
 *
 * _mongoc_read_error_get_type --
 *
 *       Checks if the error or reply from a read command is considered
 *       retryable according to the retryable reads spec. Checks both
 *       for a client error (a network exception) and a server error in
 *       the reply. @cmd_ret and @cmd_err come from the result of a
 *       read_command function.
 *
 *
 * Return:
 *       A mongoc_read_error_type_t indicating the type of error (if any).
 *
 *--------------------------------------------------------------------------
 */
mongoc_read_err_type_t
_mongoc_read_error_get_type (bool cmd_ret,
                             const bson_error_t *cmd_err,
                             const bson_t *reply)
{
   bson_error_t error;

   /* check for a client error. */
   if (!cmd_ret && cmd_err && cmd_err->domain == MONGOC_ERROR_STREAM) {
      /* Retryable reads spec: "considered retryable if [...] any network
       * exception (e.g. socket timeout or error) */
      return MONGOC_READ_ERR_RETRY;
   }

   /* check for a server error. */
   if (_mongoc_cmd_check_ok_no_wce (
          reply, MONGOC_ERROR_API_VERSION_2, &error)) {
      return MONGOC_READ_ERR_NONE;
   }

   switch (error.code) {
   case 11600: /* InterruptedAtShutdown */
   case 11602: /* InterruptedDueToReplStateChange */
   case 10107: /* NotMaster */
   case 13435: /* NotMasterNoSlaveOk */
   case 13436: /* NotMasterOrSecondary */
   case 189:   /* PrimarySteppedDown */
   case 91:    /* ShutdownInProgress */
   case 7:     /* HostNotFound */
   case 6:     /* HostUnreachable */
   case 89:    /* NetworkTimeout */
   case 9001:  /* SocketException */
      return MONGOC_READ_ERR_RETRY;
   default:
      if (strstr (error.message, "not master") ||
          strstr (error.message, "node is recovering")) {
         return MONGOC_READ_ERR_RETRY;
      }
      return MONGOC_READ_ERR_OTHER;
   }
}

void
_mongoc_error_copy_labels_and_upsert (const bson_t *src,
                                      bson_t *dst,
                                      char *label)
{
   bson_iter_t iter;
   bson_iter_t src_label;
   bson_t dst_labels;
   char str[16];
   uint32_t i = 0;
   const char *key;

   BSON_APPEND_ARRAY_BEGIN (dst, "errorLabels", &dst_labels);
   BSON_APPEND_UTF8 (&dst_labels, "0", label);

   /* append any other errorLabels already in "src" */
   if (bson_iter_init_find (&iter, src, "errorLabels") &&
       bson_iter_recurse (&iter, &src_label)) {
      while (bson_iter_next (&src_label) && BSON_ITER_HOLDS_UTF8 (&src_label)) {
         if (strcmp (bson_iter_utf8 (&src_label, NULL), label) != 0) {
            i++;
            bson_uint32_to_string (i, &key, str, sizeof str);
            BSON_APPEND_UTF8 (
               &dst_labels, key, bson_iter_utf8 (&src_label, NULL));
         }
      }
   }

   bson_append_array_end (dst, &dst_labels);
}
