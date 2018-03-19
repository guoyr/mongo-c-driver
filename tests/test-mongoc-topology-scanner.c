#include <mongoc.h>
#include <mongoc-stream-private.h>
#include <mongoc-socket-private.h>
#include <mongoc-host-list-private.h>
#include <utlist.h>

#include "mongoc-util-private.h"
#include "mongoc-client-private.h"

#include "TestSuite.h"
#include "mock_server/mock-server.h"
#include "mock_server/mock-rs.h"
#include "mock_server/future.h"
#include "mock_server/future-functions.h"
#include "test-conveniences.h"
#include "test-libmongoc.h"

#undef MONGOC_LOG_DOMAIN
#define MONGOC_LOG_DOMAIN "topology-scanner-test"

#define TIMEOUT 20000 /* milliseconds */
#define NSERVERS 10

static void
test_topology_scanner_helper (uint32_t id,
                              const bson_t *bson,
                              int64_t rtt_msec,
                              void *data,
                              const bson_error_t *error /* IN */)
{
   bson_iter_t iter;
   int *finished = (int *) data;
   uint32_t max_wire_version;

   if (error->code) {
      fprintf (stderr, "scanner error: %s\n", error->message);
      abort ();
   }

   /* mock servers are configured to return distinct wire versions */
   BSON_ASSERT (bson);
   BSON_ASSERT (bson_iter_init_find (&iter, bson, "maxWireVersion"));
   BSON_ASSERT (BSON_ITER_HOLDS_INT32 (&iter));
   max_wire_version = (uint32_t) bson_iter_int32 (&iter);
   ASSERT_CMPINT (max_wire_version, ==, id + WIRE_VERSION_MIN);

   (*finished)--;
}

static void
_test_topology_scanner (bool with_ssl)
{
   mock_server_t *servers[NSERVERS];
   mongoc_topology_scanner_t *topology_scanner;
   int i;
   bson_t q = BSON_INITIALIZER;
   int finished = NSERVERS * 3;

#ifdef MONGOC_ENABLE_SSL
   mongoc_ssl_opt_t sopt = {0};
   mongoc_ssl_opt_t copt = {0};
#endif

   topology_scanner = mongoc_topology_scanner_new (
      NULL, NULL, &test_topology_scanner_helper, &finished, TIMEOUT);

#ifdef MONGOC_ENABLE_SSL
   if (with_ssl) {
      copt.ca_file = CERT_CA;
      copt.weak_cert_validation = 1;

      mongoc_topology_scanner_set_ssl_opts (topology_scanner, &copt);
   }
#endif

   for (i = 0; i < NSERVERS; i++) {
      /* use max wire versions just to distinguish among responses */
      servers[i] = mock_server_with_autoismaster (i + WIRE_VERSION_MIN);
      mock_server_set_rand_delay (servers[i], true);

#ifdef MONGOC_ENABLE_SSL
      if (with_ssl) {
         sopt.ca_file = CERT_CA;
         sopt.pem_file = CERT_SERVER;

         mock_server_set_ssl_opts (servers[i], &sopt);
      }
#endif

      mock_server_run (servers[i]);

      mongoc_topology_scanner_add (
         topology_scanner,
         mongoc_uri_get_hosts (mock_server_get_uri (servers[i])),
         (uint32_t) i);
   }

   for (i = 0; i < 3; i++) {
      mongoc_topology_scanner_start (topology_scanner, false);
      mongoc_topology_scanner_work (topology_scanner);
      mongoc_topology_scanner_reset (topology_scanner);
   }

   BSON_ASSERT (finished == 0);

   mongoc_topology_scanner_destroy (topology_scanner);

   bson_destroy (&q);

   for (i = 0; i < NSERVERS; i++) {
      mock_server_destroy (servers[i]);
   }
}


void
test_topology_scanner (void)
{
   _test_topology_scanner (false);
}


#ifdef MONGOC_ENABLE_SSL_OPENSSL
void
test_topology_scanner_ssl (void)
{
   _test_topology_scanner (true);
}
#endif


/*
 * Servers discovered by a scan should be checked during that scan, CDRIVER-751.
 */
void
test_topology_scanner_discovery (void)
{
   mock_server_t *primary;
   mock_server_t *secondary;
   char *primary_response;
   char *secondary_response;
   mongoc_client_t *client;
   char *uri_str;
   mongoc_read_prefs_t *secondary_pref;
   bson_error_t error;
   future_t *future;
   request_t *request;
   mongoc_server_description_t *sd;

   primary = mock_server_new ();
   secondary = mock_server_new ();
   mock_server_run (primary);
   mock_server_run (secondary);

   primary_response =
      bson_strdup_printf ("{'ok': 1, "
                          " 'ismaster': true,"
                          " 'setName': 'rs',"
                          " 'minWireVersion': 2,"
                          " 'maxWireVersion': 5,"
                          " 'hosts': ['%s', '%s']}",
                          mock_server_get_host_and_port (primary),
                          mock_server_get_host_and_port (secondary));

   secondary_response =
      bson_strdup_printf ("{'ok': 1, "
                          " 'ismaster': false,"
                          " 'secondary': true,"
                          " 'setName': 'rs',"
                          " 'minWireVersion': 2,"
                          " 'maxWireVersion': 5,"
                          " 'hosts': ['%s', '%s']}",
                          mock_server_get_host_and_port (primary),
                          mock_server_get_host_and_port (secondary));

   uri_str = bson_strdup_printf ("mongodb://%s/?" MONGOC_URI_REPLICASET "=rs",
                                 mock_server_get_host_and_port (primary));
   client = mongoc_client_new (uri_str);
   secondary_pref = mongoc_read_prefs_new (MONGOC_READ_SECONDARY_PREFERRED);

   future = future_topology_select (
      client->topology, MONGOC_SS_READ, secondary_pref, &error);

   /* a single scan discovers *and* checks the secondary */
   request = mock_server_receives_ismaster (primary);
   mock_server_replies_simple (request, primary_response);
   request_destroy (request);

   /* let client process that response */
   _mongoc_usleep (250 * 1000);

   /* a check of the secondary is scheduled in this scan */
   request = mock_server_receives_ismaster (secondary);
   mock_server_replies_simple (request, secondary_response);

   /* scan completes */
   ASSERT_OR_PRINT ((sd = future_get_mongoc_server_description_ptr (future)),
                    error);

   ASSERT_CMPSTR (sd->host.host_and_port,
                  mock_server_get_host_and_port (secondary));

   mongoc_server_description_destroy (sd);
   future_destroy (future);
   request_destroy (request);
   mongoc_read_prefs_destroy (secondary_pref);
   bson_free (secondary_response);
   bson_free (primary_response);
   bson_free (uri_str);
   mongoc_client_destroy (client);
   mock_server_destroy (secondary);
   mock_server_destroy (primary);
}


/* scanner shouldn't spin if two primaries point at each other */
void
test_topology_scanner_oscillate (void)
{
   mock_server_t *server0;
   mock_server_t *server1;
   char *server0_response;
   char *server1_response;
   mongoc_client_t *client;
   mongoc_topology_scanner_t *scanner;
   char *uri_str;
   mongoc_read_prefs_t *primary_pref;
   bson_error_t error;
   future_t *future;
   request_t *request;

   server0 = mock_server_new ();
   server1 = mock_server_new ();
   mock_server_run (server0);
   mock_server_run (server1);

   /* server 0 says it's primary, but only server 1 is in the set */
   server0_response =
      bson_strdup_printf ("{'ok': 1, "
                          " 'ismaster': true,"
                          " 'setName': 'rs',"
                          " 'hosts': ['%s']}",
                          mock_server_get_host_and_port (server1));

   /* the opposite */
   server1_response =
      bson_strdup_printf ("{'ok': 1, "
                          " 'ismaster': true,"
                          " 'setName': 'rs',"
                          " 'hosts': ['%s']}",
                          mock_server_get_host_and_port (server0));

   /* start with server 0 */
   uri_str = bson_strdup_printf ("mongodb://%s/?" MONGOC_URI_REPLICASET "=rs",
                                 mock_server_get_host_and_port (server0));
   client = mongoc_client_new (uri_str);
   scanner = client->topology->scanner;
   primary_pref = mongoc_read_prefs_new (MONGOC_READ_PRIMARY);

   BSON_ASSERT (!scanner->async->ncmds);
   future = future_topology_select (
      client->topology, MONGOC_SS_READ, primary_pref, &error);

   /* a single scan discovers servers 0 and 1 */
   request = mock_server_receives_ismaster (server0);
   mock_server_replies_simple (request, server0_response);
   request_destroy (request);

   /* let client process that response */
   _mongoc_usleep (250 * 1000);

   request = mock_server_receives_ismaster (server1);
   mock_server_replies_simple (request, server1_response);

   /* we don't schedule another check of server0 */
   _mongoc_usleep (250 * 1000);

   BSON_ASSERT (!future_get_mongoc_server_description_ptr (future));
   BSON_ASSERT (scanner->async->ncmds == 0);

   future_destroy (future);
   request_destroy (request);
   mongoc_read_prefs_destroy (primary_pref);
   bson_free (server1_response);
   bson_free (server0_response);
   bson_free (uri_str);
   mongoc_client_destroy (client);
   mock_server_destroy (server1);
   mock_server_destroy (server0);
}


void
test_topology_scanner_connection_error (void)
{
   mongoc_client_t *client;
   bson_error_t error;

   /* assuming nothing is listening on this port */
   client = mongoc_client_new ("mongodb://localhost:9876");

   ASSERT (!mongoc_client_command_simple (
      client, "db", tmp_bson ("{'foo': 1}"), NULL, NULL, &error));

   ASSERT_ERROR_CONTAINS (error,
                          MONGOC_ERROR_SERVER_SELECTION,
                          MONGOC_ERROR_SERVER_SELECTION_FAILURE,
                          "connection refused calling ismaster on "
                          "'localhost:9876'");

   mongoc_client_destroy (client);
}


void
test_topology_scanner_socket_timeout (void)
{
   mock_server_t *server;
   mongoc_client_t *client;
   mongoc_uri_t *uri;
   bson_error_t error;
   char *expected_msg;

   server = mock_server_new ();
   mock_server_run (server);

   uri = mongoc_uri_copy (mock_server_get_uri (server));
   mongoc_uri_set_option_as_int32 (uri, MONGOC_URI_CONNECTTIMEOUTMS, 10);
   client = mongoc_client_new_from_uri (uri);

   ASSERT (!mongoc_client_command_simple (
      client, "db", tmp_bson ("{'foo': 1}"), NULL, NULL, &error));

   /* the mock server did accept connection, but never replied */
   expected_msg =
      bson_strdup_printf ("socket timeout calling ismaster on '%s'",
                          mongoc_uri_get_hosts (uri)->host_and_port);

   ASSERT_ERROR_CONTAINS (error,
                          MONGOC_ERROR_SERVER_SELECTION,
                          MONGOC_ERROR_SERVER_SELECTION_FAILURE,
                          expected_msg);

   bson_free (expected_msg);
   mongoc_client_destroy (client);
   mongoc_uri_destroy (uri);
   mock_server_destroy (server);
}


typedef struct {
   uint16_t slow_port;
   mongoc_client_t *client;
} initiator_data_t;


static mongoc_stream_t *
slow_initiator (const mongoc_uri_t *uri,
                const mongoc_host_list_t *host,
                void *user_data,
                bson_error_t *err)
{
   initiator_data_t *data;

   data = (initiator_data_t *) user_data;

   if (host->port == data->slow_port) {
      _mongoc_usleep (500 * 1000); /* 500 ms is longer than connectTimeoutMS */
   }

   return mongoc_client_default_stream_initiator (uri, host, data->client, err);
}


static void
test_topology_scanner_blocking_initiator (void)
{
   mock_rs_t *rs;
   mongoc_uri_t *uri;
   mongoc_client_t *client;
   initiator_data_t data;
   bson_error_t error;

   rs = mock_rs_with_autoismaster (WIRE_VERSION_MIN, /* wire version   */
                                   true,             /* has primary    */
                                   1,                /* n_secondaries  */
                                   0 /* n_arbiters     */);

   mock_rs_run (rs);
   uri = mongoc_uri_copy (mock_rs_get_uri (rs));
   mongoc_uri_set_option_as_int32 (uri, MONGOC_URI_CONNECTTIMEOUTMS, 100);
   client = mongoc_client_new_from_uri (uri);

   /* pretend last host in linked list is slow */
   data.slow_port = mongoc_uri_get_hosts (uri)->next->port;
   data.client = client;
   mongoc_client_set_stream_initiator (client, slow_initiator, &data);

   ASSERT_OR_PRINT (
      mongoc_client_command_simple (
         client, "admin", tmp_bson ("{'ismaster': 1}"), NULL, NULL, &error),
      error);

   mongoc_client_destroy (client);
   mongoc_uri_destroy (uri);
   mock_rs_destroy (rs);
}

static mock_server_t *
_mock_server_listening_on (char *server_bind_to)
{
   mock_server_t *mock_server;
   mock_server_bind_opts_t opts = {0};
   struct sockaddr_in ipv4_addr = {0};
   struct sockaddr_in6 ipv6_addr = {0};

   if (strcmp ("both", server_bind_to) == 0) {
      opts.bind_addr_len = sizeof (ipv6_addr);
      opts.family = AF_INET6;
      opts.ipv6_only = 0;
      ipv6_addr.sin6_family = AF_INET6;
      ipv6_addr.sin6_port = htons (0);   /* any port */
      ipv6_addr.sin6_addr = in6addr_any; /* either IPv4 or IPv6 */
      opts.bind_addr = (struct sockaddr_in *) &ipv6_addr;
   } else if (strcmp ("ipv4", server_bind_to) == 0) {
      opts.bind_addr_len = sizeof (ipv4_addr);
      opts.family = AF_INET;
      opts.ipv6_only = 0;
      ipv4_addr.sin_family = AF_INET;
      ipv4_addr.sin_port = htons (0);
      inet_pton (AF_INET, "127.0.0.1", &ipv4_addr.sin_addr);
      opts.bind_addr = &ipv4_addr;
   } else if (strcmp ("ipv6", server_bind_to) == 0) {
      opts.bind_addr_len = sizeof (ipv6_addr);
      opts.family = AF_INET6;
      opts.ipv6_only = 1;
      ipv6_addr.sin6_family = AF_INET6;
      ipv6_addr.sin6_port = htons (0);
      inet_pton (AF_INET6, "::1", &ipv6_addr.sin6_addr);
      opts.bind_addr = (struct sockaddr_in *) &ipv6_addr;
   } else {
      fprintf (stderr, "bad value of server_bind_to=%s\n", server_bind_to);
      ASSERT (false);
   }
   mock_server = mock_server_with_autoismaster (WIRE_VERSION_OP_MSG);
   mock_server_set_bind_opts (mock_server, &opts);
   mock_server_run (mock_server);
   return mock_server;
}

typedef struct dns_testcase {
   char *server_bind_to;  /* ipv4, ipv6, or both */
   char *client_hostname; /* 127.0.0.1, [::1], or localhost */
   bool should_succeed;
   int expected_ncmds;
   char *expected_client_bind_to; /* ipv4, ipv6, or either */
} dns_testcase_t;

static void
_test_topology_scanner_dns_helper (uint32_t id,
                                   const bson_t *bson,
                                   int64_t rtt_msec,
                                   void *data,
                                   const bson_error_t *error /* IN */)
{
   dns_testcase_t *testcase = (dns_testcase_t *) data;
   if (testcase->should_succeed) {
      ASSERT_OR_PRINT (!error->code, (*error));
   } else {
      ASSERT (error->code);
      ASSERT_ERROR_CONTAINS ((*error),
                             MONGOC_ERROR_STREAM,
                             MONGOC_ERROR_STREAM_CONNECT,
                             "connection refused");
   }
}

static void
test_topology_scanner_dns_testcase (dns_testcase_t *testcase)
{
   mongoc_host_list_t host;
   mock_server_t *server;
   mongoc_topology_scanner_t *ts;
   char *host_str;
   mongoc_socket_t *sock;
   mongoc_topology_scanner_node_t *node;

   server = _mock_server_listening_on (testcase->server_bind_to);
   ts = mongoc_topology_scanner_new (
      NULL, NULL, &_test_topology_scanner_dns_helper, testcase, TIMEOUT);
   host_str = bson_strdup_printf (
      "%s:%d", testcase->client_hostname, mock_server_get_port (server));
   _mongoc_host_list_from_string (&host, host_str);
   /* we should only have one host. */
   BSON_ASSERT (!host.next);
   bson_free (host_str);

   mongoc_topology_scanner_add (ts, &host, 1);
   mongoc_topology_scanner_scan (ts, 1 /* any server id is ok. */);
   ASSERT_CMPINT ((int) (ts->async->ncmds), ==, testcase->expected_ncmds);
   mongoc_topology_scanner_work (ts);
   node = mongoc_topology_scanner_get_node (ts, 1);

   /* check the socket that the scanner found. */
   if (testcase->should_succeed) {
      ASSERT (node->stream->type == MONGOC_STREAM_SOCKET);
      sock = mongoc_stream_socket_get_socket (
         (mongoc_stream_socket_t *) node->stream);
      if (strcmp ("ipv4", testcase->expected_client_bind_to) == 0) {
         ASSERT (sock->domain == AF_INET);
      } else if (strcmp ("ipv6", testcase->expected_client_bind_to) == 0) {
         ASSERT (sock->domain == AF_INET6);
      } else if (strcmp ("either", testcase->expected_client_bind_to) != 0) {
         fprintf (stderr,
                  "bad value for testcase->expected_client_bind_to=%s\n",
                  testcase->expected_client_bind_to);
         ASSERT (false);
      }
   }

   mongoc_topology_scanner_destroy (ts);
   mock_server_destroy (server);
}

/* test when clients try connecting to servers varying the DNS results of the
 * clients and the socket binding of the server. */
static void
test_topology_scanner_dns ()
{
   /* server can bind to: {ipv4 only, ipv6 only, both}
    * client can connect to: {127.0.0.1, ::1, localhost}
    * there are 9 combinations. */
   int ntests, i;
   dns_testcase_t tests[] = {{"ipv4", "127.0.0.1", true, 1, "ipv4"},
                             {"ipv4", "[::1]", false, 1, "n/a"},
                             {"ipv6", "127.0.0.1", false, 1, "n/a"},
                             {"ipv6", "[::1]", true, 1, "ipv6"},
                             {"both", "127.0.0.1", true, 1, "ipv4"},
                             {"both", "[::1]", true, 1, "ipv6"}};
   /* these tests require a hostname mapping to both IPv4 and IPv6 local.
 * this can be localhost normally, but some configurations may have localhost
 * only mapping to 127.0.0.1, not ::1. */
   dns_testcase_t tests_with_ipv4_and_ipv6_uri[] = {
      {"ipv4", "<placeholder>", true, 2, "ipv4"},
      {"ipv6", "<placeholder>", true, 2, "ipv6"},
      {"both", "<placeholder>", true, 2, "either"}};
   char *ipv4_and_ipv6_host =
      test_framework_getenv ("MONGOC_TEST_IPV4_AND_IPV6_HOST");

   ntests = sizeof (tests) / sizeof (dns_testcase_t);
   for (i = 0; i < ntests; ++i) {
      test_topology_scanner_dns_testcase (tests + i);
   }

   if (ipv4_and_ipv6_host) {
      ntests = sizeof (tests_with_ipv4_and_ipv6_uri) / sizeof (dns_testcase_t);
      for (i = 0; i < ntests; ++i) {
         tests_with_ipv4_and_ipv6_uri[i].client_hostname = ipv4_and_ipv6_host;
         test_topology_scanner_dns_testcase (tests_with_ipv4_and_ipv6_uri + i);
      }
      bson_free (ipv4_and_ipv6_host);
   }
}

static void
_retired_fails_to_initiate_cb (uint32_t id,
                               const bson_t *bson,
                               int64_t rtt_msec,
                               void *data,
                               const bson_error_t *error /* IN */)
{
   /* this should never get called. */
  BSON_ASSERT(false);
}

static mongoc_stream_t *
null_initiator (mongoc_async_cmd_t *acmd)
{
   return NULL;
}

/* test when a retired node fails to initiate a stream. CDRIVER-1972 introduced
 * a bug in which the topology callback would be incorrectly called when a
 * retired node failed to establish a connection.
 */
static void
test_topology_retired_fails_to_initiate ()
{
   mock_server_t *server;
   mongoc_topology_scanner_t *scanner;
   mongoc_async_cmd_t *acmd;
   mongoc_host_list_t host_list;

   server = mock_server_with_autoismaster (WIRE_VERSION_MAX);
   mock_server_run (server);

   scanner = mongoc_topology_scanner_new (
      NULL, NULL, &_retired_fails_to_initiate_cb, NULL, TIMEOUT);

   _mongoc_host_list_from_string (&host_list,
                                  mock_server_get_host_and_port (server));

   mongoc_topology_scanner_add (scanner, &host_list, 1);
   mongoc_topology_scanner_start (scanner, false);
   BSON_ASSERT (scanner->async->ncmds > 0);
   /* retire the node */
   scanner->nodes->retired = true;
   /* override the stream initiator of every async command, simulating
    * a failed mongoc_socket_new or mongoc_stream_connect. */
   DL_FOREACH (scanner->async->cmds, acmd)
   {
      scanner->async->cmds->initiator = null_initiator;
   }

   mongoc_topology_scanner_work (scanner);
   /* we expect the scanner callback not to get called. */

   mongoc_topology_scanner_destroy (scanner);
   mock_server_destroy (server);
}

void
test_topology_scanner_install (TestSuite *suite)
{
   TestSuite_AddMockServerTest (
      suite, "/TOPOLOGY/scanner", test_topology_scanner);
#ifdef MONGOC_ENABLE_SSL_OPENSSL
   TestSuite_AddMockServerTest (
      suite, "/TOPOLOGY/scanner_ssl", test_topology_scanner_ssl);
#endif
   TestSuite_AddMockServerTest (
      suite, "/TOPOLOGY/scanner_discovery", test_topology_scanner_discovery);
   TestSuite_AddMockServerTest (
      suite, "/TOPOLOGY/scanner_oscillate", test_topology_scanner_oscillate);
   TestSuite_Add (suite,
                  "/TOPOLOGY/scanner_connection_error",
                  test_topology_scanner_connection_error);
   TestSuite_AddMockServerTest (suite,
                                "/TOPOLOGY/scanner_socket_timeout",
                                test_topology_scanner_socket_timeout);
   TestSuite_AddMockServerTest (suite,
                                "/TOPOLOGY/blocking_initiator",
                                test_topology_scanner_blocking_initiator);
   TestSuite_AddMockServerTest (suite,
                                "/TOPOLOGY/dns",
                                test_topology_scanner_dns,
                                test_framework_skip_if_no_dual_ip_hostname);
   TestSuite_AddMockServerTest (suite,
                                "/TOPOLOGY/retired_fails_to_initiate",
                                test_topology_retired_fails_to_initiate);
}
