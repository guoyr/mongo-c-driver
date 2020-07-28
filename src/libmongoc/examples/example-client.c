#include <mongoc/mongoc.h>
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>

#define TRIALS 10

void * worker (void* pool_void) {
   mongoc_client_pool_t *pool;
   mongoc_client_t *client;
   bson_t ping;
   bson_error_t error;
   int i;

   bson_init (&ping);
   BCON_APPEND (&ping, "ping", BCON_INT32 (1));
   pool = (mongoc_client_pool_t *) pool_void;

   for (i = 0; i < TRIALS; i++) {
      client = mongoc_client_pool_pop (pool);
      if (!mongoc_client_command_simple (client, "db", &ping, NULL, NULL, &error)) {
         fprintf (stderr, "ping error: %s", error.message);
         exit (1);
      }
      mongoc_client_pool_push (pool, client);
   }

   bson_destroy (&ping);
   return NULL;
}

int
main (int argc, char *argv[])
{
   int nthreads;
   int i;
   pthread_t *threads;
   char *uri_str;
   mongoc_uri_t *uri;
   mongoc_client_pool_t *pool;
   bson_error_t error;

   mongoc_init ();

   if (argc != 3) {
      fprintf (stderr, "./example-client <uri> <# threads>\n");
      return EXIT_FAILURE;
   }

   uri_str = argv[1];
   nthreads = atoi (argv[2]);
   uri = mongoc_uri_new_with_error (uri_str, &error);
   if (!uri) {
      fprintf (stderr, "uri error: %s", error.message);
      return EXIT_FAILURE;
   }

   pool = mongoc_client_pool_new (uri);
   threads = (pthread_t *) bson_malloc (sizeof (pthread_t) * nthreads);
   for (i = 0; i < nthreads; i++) {
      pthread_create (threads + i, NULL /* attr */, worker, pool);
   }

   for (i = 0; i < nthreads; i++) {
      pthread_join (threads[i], NULL);
   }

   mongoc_client_pool_destroy (pool);
   mongoc_uri_destroy (uri);
   mongoc_cleanup ();
}
