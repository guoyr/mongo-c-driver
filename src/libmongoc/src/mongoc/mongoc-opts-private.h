#ifndef MONGOC_OPTS_H
#define MONGOC_OPTS_H

#include <bson/bson.h>

#include "mongoc/mongoc-client-session.h"
#include "mongoc/mongoc-bulk-operation-private.h"

/**************************************************
 *
 * Generated by build/generate-opts.py.
 *
 * DO NOT EDIT THIS FILE.
 *
 *************************************************/
/* clang-format off */

typedef struct _mongoc_crud_opts_t {
   mongoc_write_concern_t *writeConcern;
   bool write_concern_owned;
   mongoc_client_session_t *client_session;
   bson_validate_flags_t validate;
} mongoc_crud_opts_t;

typedef struct _mongoc_update_opts_t {
   mongoc_crud_opts_t crud;
   bool bypass;
   bson_t collation;
   bool upsert;
} mongoc_update_opts_t;

typedef struct _mongoc_insert_one_opts_t {
   mongoc_crud_opts_t crud;
   bool bypass;
   bson_t extra;
} mongoc_insert_one_opts_t;

typedef struct _mongoc_insert_many_opts_t {
   mongoc_crud_opts_t crud;
   bool ordered;
   bool bypass;
   bson_t extra;
} mongoc_insert_many_opts_t;

typedef struct _mongoc_delete_one_opts_t {
   mongoc_crud_opts_t crud;
   bson_t collation;
   bson_t extra;
} mongoc_delete_one_opts_t;

typedef struct _mongoc_delete_many_opts_t {
   mongoc_crud_opts_t crud;
   bson_t collation;
   bson_t extra;
} mongoc_delete_many_opts_t;

typedef struct _mongoc_update_one_opts_t {
   mongoc_update_opts_t update;
   bson_t arrayFilters;
   bson_t extra;
} mongoc_update_one_opts_t;

typedef struct _mongoc_update_many_opts_t {
   mongoc_update_opts_t update;
   bson_t arrayFilters;
   bson_t extra;
} mongoc_update_many_opts_t;

typedef struct _mongoc_replace_one_opts_t {
   mongoc_update_opts_t update;
   bson_t extra;
} mongoc_replace_one_opts_t;

typedef struct _mongoc_bulk_opts_t {
   mongoc_write_concern_t *writeConcern;
   bool write_concern_owned;
   bool ordered;
   mongoc_client_session_t *client_session;
   bson_t extra;
} mongoc_bulk_opts_t;

typedef struct _mongoc_bulk_insert_opts_t {
   bson_validate_flags_t validate;
   bson_t extra;
} mongoc_bulk_insert_opts_t;

typedef struct _mongoc_bulk_update_opts_t {
   bson_validate_flags_t validate;
   bson_t collation;
   bool upsert;
   bool multi;
} mongoc_bulk_update_opts_t;

typedef struct _mongoc_bulk_update_one_opts_t {
   mongoc_bulk_update_opts_t update;
   bson_t arrayFilters;
   bson_t extra;
} mongoc_bulk_update_one_opts_t;

typedef struct _mongoc_bulk_update_many_opts_t {
   mongoc_bulk_update_opts_t update;
   bson_t arrayFilters;
   bson_t extra;
} mongoc_bulk_update_many_opts_t;

typedef struct _mongoc_bulk_replace_one_opts_t {
   mongoc_bulk_update_opts_t update;
   bson_t extra;
} mongoc_bulk_replace_one_opts_t;

typedef struct _mongoc_bulk_remove_opts_t {
   bson_t collation;
   int32_t limit;
} mongoc_bulk_remove_opts_t;

typedef struct _mongoc_bulk_remove_one_opts_t {
   mongoc_bulk_remove_opts_t remove;
   bson_t extra;
} mongoc_bulk_remove_one_opts_t;

typedef struct _mongoc_bulk_remove_many_opts_t {
   mongoc_bulk_remove_opts_t remove;
   bson_t extra;
} mongoc_bulk_remove_many_opts_t;

typedef struct _mongoc_create_index_opts_t {
   mongoc_write_concern_t *writeConcern;
   bool write_concern_owned;
   mongoc_client_session_t *client_session;
   bson_t extra;
} mongoc_create_index_opts_t;

typedef struct _mongoc_read_write_opts_t {
   bson_t readConcern;
   mongoc_write_concern_t *writeConcern;
   bool write_concern_owned;
   mongoc_client_session_t *client_session;
   bson_t collation;
   uint32_t serverId;
   bson_t extra;
} mongoc_read_write_opts_t;

typedef struct _mongoc_gridfs_bucket_opts_t {
   const char *bucketName;
   int32_t chunkSizeBytes;
   mongoc_write_concern_t *writeConcern;
   bool write_concern_owned;
   mongoc_read_concern_t *readConcern;
   bson_t extra;
} mongoc_gridfs_bucket_opts_t;

typedef struct _mongoc_gridfs_bucket_upload_opts_t {
   int32_t chunkSizeBytes;
   bson_t metadata;
   bson_t extra;
} mongoc_gridfs_bucket_upload_opts_t;

typedef struct _mongoc_client_side_encryption_opts_t {
   bson_t schemas;
   const char *awsRegion;
   const char *awsSecretAccessKey;
   const char *awsAccessKeyId;
   const char *mongocryptdURI;
   bool useRemoteSchemas;
   bson_t extra;
} mongoc_client_side_encryption_opts_t;

bool
_mongoc_insert_one_opts_parse (
   mongoc_client_t *client,
   const bson_t *opts,
   mongoc_insert_one_opts_t *mongoc_insert_one_opts,
   bson_error_t *error);

void
_mongoc_insert_one_opts_cleanup (mongoc_insert_one_opts_t *mongoc_insert_one_opts);

bool
_mongoc_insert_many_opts_parse (
   mongoc_client_t *client,
   const bson_t *opts,
   mongoc_insert_many_opts_t *mongoc_insert_many_opts,
   bson_error_t *error);

void
_mongoc_insert_many_opts_cleanup (mongoc_insert_many_opts_t *mongoc_insert_many_opts);

bool
_mongoc_delete_one_opts_parse (
   mongoc_client_t *client,
   const bson_t *opts,
   mongoc_delete_one_opts_t *mongoc_delete_one_opts,
   bson_error_t *error);

void
_mongoc_delete_one_opts_cleanup (mongoc_delete_one_opts_t *mongoc_delete_one_opts);

bool
_mongoc_delete_many_opts_parse (
   mongoc_client_t *client,
   const bson_t *opts,
   mongoc_delete_many_opts_t *mongoc_delete_many_opts,
   bson_error_t *error);

void
_mongoc_delete_many_opts_cleanup (mongoc_delete_many_opts_t *mongoc_delete_many_opts);

bool
_mongoc_update_one_opts_parse (
   mongoc_client_t *client,
   const bson_t *opts,
   mongoc_update_one_opts_t *mongoc_update_one_opts,
   bson_error_t *error);

void
_mongoc_update_one_opts_cleanup (mongoc_update_one_opts_t *mongoc_update_one_opts);

bool
_mongoc_update_many_opts_parse (
   mongoc_client_t *client,
   const bson_t *opts,
   mongoc_update_many_opts_t *mongoc_update_many_opts,
   bson_error_t *error);

void
_mongoc_update_many_opts_cleanup (mongoc_update_many_opts_t *mongoc_update_many_opts);

bool
_mongoc_replace_one_opts_parse (
   mongoc_client_t *client,
   const bson_t *opts,
   mongoc_replace_one_opts_t *mongoc_replace_one_opts,
   bson_error_t *error);

void
_mongoc_replace_one_opts_cleanup (mongoc_replace_one_opts_t *mongoc_replace_one_opts);

bool
_mongoc_bulk_opts_parse (
   mongoc_client_t *client,
   const bson_t *opts,
   mongoc_bulk_opts_t *mongoc_bulk_opts,
   bson_error_t *error);

void
_mongoc_bulk_opts_cleanup (mongoc_bulk_opts_t *mongoc_bulk_opts);

bool
_mongoc_bulk_insert_opts_parse (
   mongoc_client_t *client,
   const bson_t *opts,
   mongoc_bulk_insert_opts_t *mongoc_bulk_insert_opts,
   bson_error_t *error);

void
_mongoc_bulk_insert_opts_cleanup (mongoc_bulk_insert_opts_t *mongoc_bulk_insert_opts);

bool
_mongoc_bulk_update_one_opts_parse (
   mongoc_client_t *client,
   const bson_t *opts,
   mongoc_bulk_update_one_opts_t *mongoc_bulk_update_one_opts,
   bson_error_t *error);

void
_mongoc_bulk_update_one_opts_cleanup (mongoc_bulk_update_one_opts_t *mongoc_bulk_update_one_opts);

bool
_mongoc_bulk_update_many_opts_parse (
   mongoc_client_t *client,
   const bson_t *opts,
   mongoc_bulk_update_many_opts_t *mongoc_bulk_update_many_opts,
   bson_error_t *error);

void
_mongoc_bulk_update_many_opts_cleanup (mongoc_bulk_update_many_opts_t *mongoc_bulk_update_many_opts);

bool
_mongoc_bulk_replace_one_opts_parse (
   mongoc_client_t *client,
   const bson_t *opts,
   mongoc_bulk_replace_one_opts_t *mongoc_bulk_replace_one_opts,
   bson_error_t *error);

void
_mongoc_bulk_replace_one_opts_cleanup (mongoc_bulk_replace_one_opts_t *mongoc_bulk_replace_one_opts);

bool
_mongoc_bulk_remove_one_opts_parse (
   mongoc_client_t *client,
   const bson_t *opts,
   mongoc_bulk_remove_one_opts_t *mongoc_bulk_remove_one_opts,
   bson_error_t *error);

void
_mongoc_bulk_remove_one_opts_cleanup (mongoc_bulk_remove_one_opts_t *mongoc_bulk_remove_one_opts);

bool
_mongoc_bulk_remove_many_opts_parse (
   mongoc_client_t *client,
   const bson_t *opts,
   mongoc_bulk_remove_many_opts_t *mongoc_bulk_remove_many_opts,
   bson_error_t *error);

void
_mongoc_bulk_remove_many_opts_cleanup (mongoc_bulk_remove_many_opts_t *mongoc_bulk_remove_many_opts);

bool
_mongoc_create_index_opts_parse (
   mongoc_client_t *client,
   const bson_t *opts,
   mongoc_create_index_opts_t *mongoc_create_index_opts,
   bson_error_t *error);

void
_mongoc_create_index_opts_cleanup (mongoc_create_index_opts_t *mongoc_create_index_opts);

bool
_mongoc_read_write_opts_parse (
   mongoc_client_t *client,
   const bson_t *opts,
   mongoc_read_write_opts_t *mongoc_read_write_opts,
   bson_error_t *error);

void
_mongoc_read_write_opts_cleanup (mongoc_read_write_opts_t *mongoc_read_write_opts);

bool
_mongoc_gridfs_bucket_opts_parse (
   mongoc_client_t *client,
   const bson_t *opts,
   mongoc_gridfs_bucket_opts_t *mongoc_gridfs_bucket_opts,
   bson_error_t *error);

void
_mongoc_gridfs_bucket_opts_cleanup (mongoc_gridfs_bucket_opts_t *mongoc_gridfs_bucket_opts);

bool
_mongoc_gridfs_bucket_upload_opts_parse (
   mongoc_client_t *client,
   const bson_t *opts,
   mongoc_gridfs_bucket_upload_opts_t *mongoc_gridfs_bucket_upload_opts,
   bson_error_t *error);

void
_mongoc_gridfs_bucket_upload_opts_cleanup (mongoc_gridfs_bucket_upload_opts_t *mongoc_gridfs_bucket_upload_opts);

bool
_mongoc_client_side_encryption_opts_parse (
   mongoc_client_t *client,
   const bson_t *opts,
   mongoc_client_side_encryption_opts_t *mongoc_client_side_encryption_opts,
   bson_error_t *error);

void
_mongoc_client_side_encryption_opts_cleanup (mongoc_client_side_encryption_opts_t *mongoc_client_side_encryption_opts);

#endif /* MONGOC_OPTS_H */
