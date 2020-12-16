/*
 * Copyright 2020-present MongoDB, Inc.
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

#include "entity-map.h"

#include "bson-parser.h"
#include "TestSuite.h"
#include "test-conveniences.h"
#include "test-libmongoc.h"
#include "utlist.h"

/* TODO: remove this include once CDRIVER-3285 is resolved. */
#include "mongoc-uri-private.h"

static void
entity_destroy (entity_t *entity);

entity_map_t *
entity_map_new ()
{
   return bson_malloc0 (sizeof (entity_map_t));
}

void
entity_map_destroy (entity_map_t *entity_map)
{
   entity_t *entity, *tmp;
   LL_FOREACH_SAFE (entity_map->entities, entity, tmp)
   {
      entity_destroy (entity);
   }
   bson_free (entity_map);
}

static bool
uri_apply_options (mongoc_uri_t *uri, bson_t *opts, bson_error_t *error)
{
   bson_iter_t iter;
   bool ret = false;
   bool wcSet = false;
   mongoc_write_concern_t *wc;

   /* There may be multiple URI options (w, wTimeoutMS, journal) for a write
    * concern. Parse all options before setting the write concern on the URI. */
   wc = mongoc_write_concern_new ();

   BSON_FOREACH (opts, iter)
   {
      const char *key;

      key = bson_iter_key (&iter);

      if (0 == strcmp ("readConcernLevel", key)) {
         mongoc_read_concern_t *rc;

         rc = mongoc_read_concern_new ();
         mongoc_read_concern_set_level (rc, bson_iter_utf8 (&iter, NULL));
         mongoc_uri_set_read_concern (uri, rc);
         mongoc_read_concern_destroy (rc);
      } else if (0 == strcmp ("w", key)) {
         wcSet = true;
         mongoc_write_concern_set_w (wc, bson_iter_int32 (&iter));
      } else if (mongoc_uri_option_is_int32 (key)) {
         mongoc_uri_set_option_as_int32 (uri, key, bson_iter_int32 (&iter));
      } else if (mongoc_uri_option_is_int64 (key)) {
         mongoc_uri_set_option_as_int64 (uri, key, bson_iter_int64 (&iter));
      } else if (mongoc_uri_option_is_bool (key)) {
         mongoc_uri_set_option_as_bool (uri, key, bson_iter_bool (&iter));
      } else {
         test_set_error (
            error, "Unimplemented test runner support for URI option: %s", key);
         goto done;
      }
   }

   if (wcSet) {
      mongoc_uri_set_write_concern (uri, wc);
   }

   ret = true;

done:
   mongoc_write_concern_destroy (wc);
   return ret;
}

event_t *
event_new (char *type)
{
   event_t *event;

   event = bson_malloc0 (sizeof (event_t));
   event->type = bson_strdup (type);
   return event;
}

void
event_destroy (event_t *event)
{
   if (!event) {
      return;
   }

   bson_destroy (event->command);
   bson_destroy (event->reply);
   bson_free (event->type);
   bson_free (event);
}

entity_t *
entity_new (char *type)
{
   entity_t *entity;
   entity = bson_malloc0 (sizeof (entity_t));
   entity->type = bson_strdup (type);
   return entity;
}

bool
should_ignore_event (entity_t *client_entity, event_t *event)
{
   bson_iter_t iter;

   if (0 == strcmp (event->command_name, "configureFailPoint")) {
      return true;
   }

   if (!client_entity->ignore_command_monitoring_events) {
      return false;
   }

   BSON_FOREACH (client_entity->ignore_command_monitoring_events, iter)
   {
      if (0 == strcmp (event->command_name, bson_iter_utf8 (&iter, NULL))) {
         return true;
      }
   }

   return false;
}

static void
command_started (const mongoc_apm_command_started_t *started)
{
   entity_t *entity;
   event_t *event;

   entity = (entity_t *) mongoc_apm_command_started_get_context (started);
   event = event_new ("commandStarted");
   event->command =
      bson_copy (mongoc_apm_command_started_get_command (started));
   event->command_name =
      bson_strdup (mongoc_apm_command_started_get_command_name (started));

   if (should_ignore_event (entity, event)) {
      event_destroy (event);
      return;
   }

   LL_APPEND (entity->events, event);
}

static void
command_failed (const mongoc_apm_command_failed_t *failed)
{
   entity_t *entity;
   event_t *event;

   entity = (entity_t *) mongoc_apm_command_failed_get_context (failed);
   event = event_new ("commandFailed");
   event->reply = bson_copy (mongoc_apm_command_failed_get_reply (failed));
   event->command_name =
      bson_strdup (mongoc_apm_command_failed_get_command_name (failed));

   if (should_ignore_event (entity, event)) {
      event_destroy (event);
      return;
   }
   LL_APPEND (entity->events, event);
}

static void
command_succeeded (const mongoc_apm_command_succeeded_t *succeeded)
{
   entity_t *entity;
   event_t *event;

   entity = (entity_t *) mongoc_apm_command_succeeded_get_context (succeeded);
   event = event_new ("commandSucceeded");
   event->reply =
      bson_copy (mongoc_apm_command_succeeded_get_reply (succeeded));
   event->command_name =
      bson_strdup (mongoc_apm_command_succeeded_get_command_name (succeeded));

   if (should_ignore_event (entity, event)) {
      event_destroy (event);
      return;
   }
   LL_APPEND (entity->events, event);
}

entity_t *
entity_client_new (bson_t *bson, bson_error_t *error)
{
   bson_parser_t *parser;
   entity_t *entity;
   mongoc_client_t *client;
   mongoc_uri_t *uri = NULL;
   bool ret = false;
   bson_iter_t iter;
   mongoc_apm_callbacks_t *callbacks = NULL;
   bson_t *uri_options = NULL;
   bool *use_multiple_mongoses = NULL;
   bson_t *observe_events = NULL;

   entity = entity_new ("client");
   parser = bson_parser_new ();
   bson_parser_utf8 (parser, "id", &entity->id);
   bson_parser_doc_optional (parser, "uriOptions", &uri_options);
   bson_parser_bool_optional (
      parser, "useMultipleMongoses", &use_multiple_mongoses);
   bson_parser_array_optional (parser, "observeEvents", &observe_events);
   bson_parser_array_optional (parser,
                               "ignoreCommandMonitoringEvents",
                               &entity->ignore_command_monitoring_events);

   if (!bson_parser_parse (parser, bson, error)) {
      goto done;
   }

   /* Build the client's URI. */
   uri = test_framework_get_uri ();
   if (use_multiple_mongoses && test_framework_is_mongos ()) {
      /* TODO: Once CDRIVER-3285 is resolved, update this to no longer rely on
       * the private URI API. */
      if (*use_multiple_mongoses) {
         if (!mongoc_uri_upsert_host_and_port (uri, "localhost:27017", error)) {
            goto done;
         }
         if (!mongoc_uri_upsert_host_and_port (uri, "localhost:27018", error)) {
            goto done;
         }
      } else {
         const mongoc_host_list_t *hosts;

         hosts = mongoc_uri_get_hosts (uri);
         if (hosts->next) {
            test_set_error (error,
                            "useMultiMongoses is false, so expected single "
                            "host listed, but got: %s",
                            mongoc_uri_get_string (uri));
            goto done;
         }
      }
   }

   if (uri_options) {
      /* Apply URI options. */
      if (!uri_apply_options (uri, uri_options, error)) {
         goto done;
      }
   }

   client = mongoc_client_new_from_uri (uri);
   entity->value = client;
   callbacks = mongoc_apm_callbacks_new ();

   if (observe_events) {
      BSON_FOREACH (observe_events, iter)
      {
         const char *event_type = bson_iter_utf8 (&iter, NULL);

         if (0 == strcmp (event_type, "commandStartedEvent")) {
            mongoc_apm_set_command_started_cb (callbacks, command_started);
         } else if (0 == strcmp (event_type, "commandFailedEvent")) {
            mongoc_apm_set_command_failed_cb (callbacks, command_failed);
         } else if (0 == strcmp (event_type, "commandSucceededEvent")) {
            mongoc_apm_set_command_succeeded_cb (callbacks, command_succeeded);
         } else {
            test_set_error (error, "Unexpected event type: %s", event_type);
            goto done;
         }
      }
   }
   mongoc_client_set_apm_callbacks (client, callbacks, entity);

   ret = true;
done:
   mongoc_uri_destroy (uri);
   bson_parser_destroy (parser);
   mongoc_apm_callbacks_destroy (callbacks);
   bson_destroy (uri_options);
   bson_free (use_multiple_mongoses);
   bson_destroy (observe_events);
   if (!ret) {
      entity_destroy (entity);
      return NULL;
   }
   return entity;
}

entity_t *
entity_database_new (entity_map_t *entity_map,
                     bson_t *bson,
                     bson_error_t *error)
{
   bson_parser_t *parser;
   entity_t *entity;
   const entity_t *client_entity;
   char *client_id = NULL;
   mongoc_client_t *client;
   char *database_name = NULL;
   bool ret = false;

   entity = entity_new ("database");
   parser = bson_parser_new ();
   bson_parser_utf8 (parser, "id", &entity->id);
   bson_parser_utf8 (parser, "client", &client_id);
   bson_parser_utf8 (parser, "databaseName", &database_name);

   if (!bson_parser_parse (parser, bson, error)) {
      goto done;
   }

   client_entity = entity_map_get (entity_map, client_id, error);
   if (!client_entity) {
      goto done;
   }

   client = (mongoc_client_t *) client_entity->value;
   entity->value = (void *) mongoc_client_get_database (client, database_name);

   ret = true;
done:
   bson_free (client_id);
   bson_free (database_name);
   bson_parser_destroy (parser);
   if (!ret) {
      entity_destroy (entity);
      return NULL;
   }
   return entity;
}

entity_t *
entity_collection_new (entity_map_t *entity_map,
                       bson_t *bson,
                       bson_error_t *error)
{
   bson_parser_t *parser;
   entity_t *entity;
   entity_t *database_entity;
   mongoc_database_t *database;
   bool ret = false;
   char *database_id = NULL;
   char *collection_name = NULL;

   entity = entity_new ("collection");
   parser = bson_parser_new ();
   bson_parser_utf8 (parser, "id", &entity->id);
   bson_parser_utf8 (parser, "database", &database_id);
   bson_parser_utf8 (parser, "collectionName", &collection_name);
   if (!bson_parser_parse (parser, bson, error)) {
      goto done;
   }

   database_entity = entity_map_get (entity_map, database_id, error);
   database = (mongoc_database_t *) database_entity->value;
   entity->value = mongoc_database_get_collection (database, collection_name);
   ret = true;
done:
   bson_free (collection_name);
   bson_free (database_id);
   bson_parser_destroy (parser);
   if (!ret) {
      entity_destroy (entity);
      return NULL;
   }
   return entity;
}

mongoc_session_opt_t *
session_opts_new (bson_t *bson, bson_error_t *error)
{
   bool ret = false;
   mongoc_session_opt_t *opts = NULL;
   bson_parser_t *parser;
   bool *causal_consistency = NULL;

   parser = bson_parser_new ();
   bson_parser_bool_optional (parser, "causalConsistency", &causal_consistency);
   if (!bson_parser_parse (parser, bson, error)) {
      goto done;
   }

   opts = mongoc_session_opts_new ();
   if (causal_consistency) {
      mongoc_session_opts_set_causal_consistency (opts, *causal_consistency);
   }

done:
   bson_parser_destroy (parser);
   bson_free (causal_consistency);
   if (!ret) {
      mongoc_session_opts_destroy (opts);
      return NULL;
   }
   return opts;
}

entity_t *
entity_session_new (entity_map_t *entity_map, bson_t *bson, bson_error_t *error)
{
   bson_parser_t *parser;
   entity_t *entity;
   entity_t *client_entity;
   mongoc_client_t *client;
   char *client_id = NULL;
   bson_t *session_opts_bson = NULL;
   mongoc_session_opt_t *session_opts = NULL;
   bool ret = false;

   entity = entity_new ("session");
   parser = bson_parser_new ();
   bson_parser_utf8 (parser, "id", &entity->id);
   bson_parser_utf8 (parser, "client", &client_id);
   bson_parser_doc (parser, "sessionOptions", &session_opts_bson);
   if (!bson_parser_parse (parser, bson, error)) {
      goto done;
   }

   client_entity = entity_map_get (entity_map, client_id, error);
   client = (mongoc_client_t *) client_entity->value;
   session_opts = session_opts_new (session_opts_bson, error);
   if (!session_opts) {
      goto done;
   }
   entity->value = mongoc_client_start_session (client, session_opts, error);
   if (!entity->value) {
      goto done;
   }
   ret = true;
done:
   mongoc_session_opts_destroy (session_opts);
   bson_free (client_id);
   bson_destroy (session_opts_bson);
   bson_parser_destroy (parser);
   if (!ret) {
      entity_destroy (entity);
      return NULL;
   }
   return entity;
}

/* Caveat: The spec encourages, but does not require, that entities are defined
 * in dependency order:
 * "Test files SHOULD define entities in dependency order, such that all
 * referenced entities (e.g. client) are defined before any of their dependent
 * entities (e.g. database, session)."
 * If a test ever does break this pattern (flipping dependency order), that can
 * be solved by creating C objects lazily in entity_map_get.
 * The current implementation here does the simple thing and creates the C
 * object immediately.
 */
bool
entity_map_create (entity_map_t *entity_map, bson_t *bson, bson_error_t *error)
{
   bson_iter_t iter;
   const char *entity_type;
   bson_t entity_bson;
   entity_t *entity = NULL;
   entity_t *entity_iter;
   bool ret = false;

   bson_iter_init (&iter, bson);
   if (!bson_iter_next (&iter)) {
      test_set_error (error, "Empty entity");
      goto done;
   }

   entity_type = bson_iter_key (&iter);
   bson_iter_bson (&iter, &entity_bson);
   if (bson_iter_next (&iter)) {
      test_set_error (error,
                      "Extra field in entity: %s: %s",
                      bson_iter_key (&iter),
                      tmp_json (bson));
      goto done;
   }

   if (0 == strcmp (entity_type, "client")) {
      entity = entity_client_new (&entity_bson, error);
   } else if (0 == strcmp (entity_type, "database")) {
      entity = entity_database_new (entity_map, &entity_bson, error);
   } else if (0 == strcmp (entity_type, "collection")) {
      entity = entity_collection_new (entity_map, &entity_bson, error);
   } else if (0 == strcmp (entity_type, "session")) {
      entity = entity_session_new (entity_map, &entity_bson, error);
   } else {
      test_set_error (
         error, "Unknown entity type: %s: %s", entity_type, tmp_json (bson));
      goto done;
   }

   if (!entity) {
      goto done;
   }

   LL_FOREACH (entity_map->entities, entity_iter)
   {
      if (0 == strcmp (entity_iter->id, entity->id)) {
         test_set_error (
            error, "Attempting to create duplicate entity: '%s'", entity->id);
         entity_destroy (entity);
         goto done;
      }
   }

   ret = true;
done:
   if (!ret) {
      entity_destroy (entity);
   } else {
      LL_PREPEND (entity_map->entities, entity);
   }
   return ret;
}

static void
entity_destroy (entity_t *entity)
{
   event_t *event;
   event_t *tmp;

   if (!entity) {
      return;
   }
   if (entity->type) {
      if (0 == strcmp ("client", entity->type)) {
         mongoc_client_t *client;

         client = (mongoc_client_t *) entity->value;
         mongoc_client_destroy (client);
      } else if (0 == strcmp ("database", entity->type)) {
         mongoc_database_t *db;

         db = (mongoc_database_t *) entity->value;
         mongoc_database_destroy (db);
      } else if (0 == strcmp ("collection", entity->type)) {
         mongoc_collection_t *coll;

         coll = (mongoc_collection_t *) entity->value;
         mongoc_collection_destroy (coll);
      } else if (0 == strcmp ("session", entity->type)) {
         mongoc_client_session_t *sess;

         sess = (mongoc_client_session_t *) entity->value;
         mongoc_client_session_destroy (sess);
      }
   }

   LL_FOREACH_SAFE (entity->events, event, tmp)
   {
      event_destroy (event);
   }

   bson_destroy (entity->ignore_command_monitoring_events);
   bson_free (entity->type);
   bson_free (entity->id);
   bson_free (entity);
}

entity_t *
entity_map_get (entity_map_t *entity_map, const char *id, bson_error_t *error)
{
   entity_t *entity;
   LL_FOREACH (entity_map->entities, entity)
   {
      if (0 == strcmp (entity->id, id)) {
         break;
      }
   }

   if (NULL == entity) {
      test_set_error (error, "Entity '%s' not found", id);
      return NULL;
   }

   return entity;
}
