/*
 * Copyright (c) 2016 Sound <sound@sagaforce.com>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <stdarg.h>
#include <time.h>
#include <errno.h>
#include "sha256.h"
#include "json.h"
#include "compat/string.h"
#include "os/filesystem.h"
#include "os/mutex.h"
#include "config.h"
#include "socket_io.h"
#include "mkdir_recusive.h"
#include "git_lfs_server.h"

typedef enum git_lfs_operation_type
{
	git_lfs_operation_unknown,
	git_lfs_operation_upload,
	git_lfs_operation_download
} git_lfs_operation;

// oid's are hashes, only sha256 is defined
static int is_valid_oid(const char *oid)
{
	if(strnlen(oid, 65) != 64) return 0;
	
	for(int i = 0; i < 64; i++) {
		if(!((oid[i] >= '0' && oid[i] <= '9') ||
		   (oid[i] >='a' && oid[i] <= 'f')))
		{
			return 0;
		}
	}
	
	return 1;
}

void git_lfs_write_error(const struct socket_io *io, int error_code, const char *format, ...)
{
	char message[4096];
	va_list va;
	va_start(va, format);
	vsnprintf(message, sizeof(message), format, va);
	va_end(va);

	const char *error_reason = "Unknown error";
	switch(error_code) {
		case 400: error_reason = "Bad Request"; break;
		case 404: error_reason = "Not Found"; break;
		case 422: error_reason = "Unprocessable Entity"; break;
		case 500: error_reason = "Internal Server Error"; break;
		case 501: error_reason = "Not Implemented"; break;
	}
	
	io->write_http_status(io->context, error_code, error_reason);
	
	json_object *error = json_object_new_object();
	json_object_object_add(error, "message", json_object_new_string(message));
	
	char content_length[64];
	const char *body = json_object_get_string(error);
	int length = strlen(body);
	
	snprintf(content_length, sizeof(content_length), "Content-Length: %d", length);
	const char *headers[] = {
		"Content-Type: application/vnd.git-lfs+json",
		content_length
	};
	
	io->write_headers(io->context, headers, sizeof(headers) / sizeof(headers[0]));
	io->write(io->context, body, length);
	
	json_object_put(error);
}

static void git_lfs_server_handle_batch(const struct git_lfs_config *config, const struct git_lfs_repo *repo, const struct socket_io *io)
{
	char buffer[4096];
	int n;
	json_tokener * tokener = json_tokener_new();
	struct json_object *root = NULL;
	
	while((n = io->read(io->context, buffer, sizeof(buffer))) > 0)
	{
		root = json_tokener_parse_ex(tokener, buffer, n);
		enum json_tokener_error err = json_tokener_get_error(tokener);
		if(err == json_tokener_success) break;
		if(err != json_tokener_continue) {
			git_lfs_write_error(io, 400, "JSON parsing error.");
			goto error0;
		}
	}
	
	if(config->verbose >= 2)
	{
		printf("> %s\n", json_object_get_string(root));
	}
	
	struct json_object *operation;
	if(!json_object_object_get_ex(root, "operation", &operation) ||
	   !json_object_is_type(operation, json_type_string))
	{
		git_lfs_write_error(io, 400, "API error. Missing operation.");
		goto error0;
	}
	
	
	git_lfs_operation op = git_lfs_operation_unknown;
	const char *operation_string = json_object_get_string(operation);
	if(strcmp(operation_string, "upload") == 0) {
		op = git_lfs_operation_upload;
	} else if(strcmp(operation_string, "download") == 0) {
		op = git_lfs_operation_download;
	} else {
		git_lfs_write_error(io, 400, "Unknown operation.");
		goto error0;
	}
	
	struct json_object *objects;
	if(!json_object_object_get_ex(root, "objects", &objects)
	   || !json_object_is_type(objects, json_type_array))
	{
		git_lfs_write_error(io, 400, "API error. Missing objects.");
		goto error0;
	}
	
	struct json_object *response_object = json_object_new_object();
	struct json_object *output_objects = json_object_new_array();
	
	struct array_list *obj_list = json_object_get_array(objects);
	int obj_count = array_list_length(obj_list);
	for(int i = 0; i < obj_count; i++) {
		struct json_object * obj = array_list_get_idx(obj_list, i);
		if(!json_object_is_type(obj, json_type_object)) {
			git_lfs_write_error(io, 400, "API error. Invalid object in stream.");
			goto error1;
		}
		
		struct json_object *oid, *size;
		if(!json_object_object_get_ex(obj, "oid", &oid) ||
		   !json_object_is_type(oid, json_type_string) ||
		   !json_object_object_get_ex(obj, "size", &size) ||
		   !json_object_is_type(size, json_type_int))
		{
			git_lfs_write_error(io, 400, "API error. Missing oid and size.");
			goto error1;
		}
		
		struct json_object *obj_info = json_object_new_object();
		json_object_object_add(obj_info, "oid", json_object_get(oid));
		json_object_object_add(obj_info, "size", json_object_get(size));
		json_object_object_add(obj_info, "authenticated", json_object_new_boolean(1));
		struct json_object *actions = json_object_new_object();
		
		const char *oid_str = json_object_get_string(oid);
		if(!oid_str) {
			git_lfs_write_error(io, 400, "OID is not a string.");
			goto error1;
		}

		//char expire_time[32];
		//strftime(expire_time, sizeof(expire_time), "%FT%TZ", gmtime(&access_token->expire));
		
		char object_path[PATH_MAX];
		if(snprintf(object_path, sizeof(object_path), "%s/%.2s/%s", repo->root_dir, oid_str, oid_str) >= sizeof(object_path)) {
			git_lfs_write_error(io, 400, "Object path is too long.");
			goto error1;
		}
		
		switch(op) {
			case git_lfs_operation_upload:
			{
				if(!os_file_exists(object_path)) // only add upload entry if file doesn't exist
				{
					char url[1024];

					// add upload url
					if(snprintf(url, sizeof(url), "%s/%s/upload/%s", config->base_url, repo->uri, oid_str) >= (long)sizeof(url)) {
						git_lfs_write_error(io, 400, "Upload URL is too long.");
						json_object_put(actions);
						json_object_put(obj_info);
						goto error1;
					}

					struct json_object *upload = json_object_new_object();
					json_object_object_add(upload, "href", json_object_new_string(url));
					//json_object_object_add(upload, "expires_at", json_object_new_string(expire_time));
					json_object_object_add(actions, "upload", upload);
					
					// add verify url
					if(snprintf(url, sizeof(url), "%s/%s/verify", config->base_url, repo->uri) >= (long)sizeof(url)) {
						git_lfs_write_error(io, 400, "Upload URL is too long.");
						json_object_put(actions);
						json_object_put(obj_info);
						goto error1;
					}
					
					struct json_object *verify = json_object_new_object();
					json_object_object_add(verify, "href", json_object_new_string(url));
					//json_object_object_add(verify, "expires_at", json_object_new_string(expire_time));
					json_object_object_add(actions, "verify", verify);
					json_object_object_add(obj_info, "actions", actions);
				}

				break;
			}
			
			case git_lfs_operation_download:
			{
				if(!os_file_exists(object_path)) {
					struct json_object *error = json_object_new_object();
					json_object_object_add(error, "code", json_object_new_int(404));
					json_object_object_add(error, "message", json_object_new_string("Object does not exist"));
					json_object_object_add(obj_info, "error", error);
				} else {
					char download_url[1024];
					
					if(snprintf(download_url, sizeof(download_url), "%s/%s/download/%s", config->base_url, repo->uri, oid_str) >= (long)sizeof(download_url)) {
						git_lfs_write_error(io, 400, "Download URL is too long.");
						json_object_put(actions);
						json_object_put(obj_info);
						goto error1;
					}
					
					struct json_object *download = json_object_new_object();
					json_object_object_add(download, "href", json_object_new_string(download_url));
					//json_object_object_add(download, "expires_at", json_object_new_string(expire_time));
					json_object_object_add(actions, "download", download);
					json_object_object_add(obj_info, "actions", actions);
				}
				break;
			}
			
			default:
				break;
		}

		json_object_array_add(output_objects, obj_info);
	}

	json_object_object_add(response_object, "objects", json_object_get(output_objects));

	const char *response_json = json_object_get_string(response_object);
	int length = strlen(response_json);
	char content_length[64];
	snprintf(content_length, sizeof(content_length), "Content-Length: %d", length);
	const char *headers[] = {
		"Content-Type: application/vnd.git-lfs+json",
		content_length
	};

	io->write_http_status(io->context, 200, "OK");
	io->write_headers(io->context, headers, sizeof(headers) / sizeof(headers[0]));
	io->write(io->context, response_json, strlen(response_json));
	
	if(config->verbose >= 2)
	{
		printf("< %s\n", response_json);
	}
	
error1:
	json_object_put(output_objects);
	json_object_put(response_object);
error0:
	if(root) json_object_put(root);
	json_tokener_free(tokener);
}

static void git_lfs_download(const struct git_lfs_config *config, const struct git_lfs_repo *repo, const struct socket_io *io, const char *oid)
{
	char buffer[4096];
	int n;
	char object_path[PATH_MAX];

	if(!is_valid_oid(oid)) {
		git_lfs_write_error(io, 400, "Object ID is not valid.");
		return;
	}

	if(snprintf(object_path, sizeof(object_path), "%s/%.2s/%s", repo->root_dir, oid, oid) >= (long)sizeof(object_path))
	{
		git_lfs_write_error(io, 400, "Object path is too long.");
		return;
	}

	FILE *fp = fopen(object_path, "rb");
	if(!fp) {
		git_lfs_write_error(io, 404, "Object was not found.");
		return;
	}
	
	long filesize = os_file_size(object_path);
	if(filesize < 0) {
		git_lfs_write_error(io, 400, "Could not determine file size.");
		fclose(fp);
		return;
	}

	char content_length[64];
	snprintf(content_length, sizeof(content_length), "Content-Length: %ld", filesize);
	const char *headers[] = {
		"Content-Type: application/octet-stream",
		content_length
	};

	io->write_http_status(io->context, 200, "OK");
	io->write_headers(io->context, headers, sizeof(headers) / sizeof(headers[0]));

	while((n = fread(buffer, 1, sizeof(buffer), fp)) > 0) {
		io->write(io->context, buffer, n);
	}
	
	fclose(fp);
}

static unsigned char hex_to_int(char hex)
{
	if(hex >= 'a' && hex <= 'f') {
		return 10 + hex - 'a';
	} else if(hex >= 'A' && hex <= 'F') {
		return 10 + hex - 'A';
	} else if(hex >= '0' && hex <= '9') {
		return hex - '0';
	}
	return 0;
}

static void git_lfs_upload(const struct git_lfs_config *config, const struct git_lfs_repo *repo, const struct socket_io *io, const char *oid)
{
	char buffer[4096];
	int n;
	char object_path[PATH_MAX], tmp_object_path[PATH_MAX];

	if(snprintf(object_path, sizeof(object_path), "%s/%.2s/", repo->root_dir, oid) >= (long)sizeof(object_path))
	{
		git_lfs_write_error(io, 400, "Object path is too long.");
		return;
	}

	// object_path contains the upload directory
	if(!os_file_exists(object_path))
	{
		mkdir_recursive(object_path, 0700);
	}

	// append the oid
	if(strlcat(object_path, oid, sizeof(object_path)) >= sizeof(object_path))
	{
		git_lfs_write_error(io, 400, "Object path is too long.");
		return;
	}

	if(strlcpy(tmp_object_path, object_path, sizeof(tmp_object_path)) >= sizeof(tmp_object_path) ||
	   strlcat(tmp_object_path, "-tmp", sizeof(tmp_object_path)) >= sizeof(tmp_object_path))
	{
		git_lfs_write_error(io, 400, "Object path is too long.");
		return;
	}


	FILE *fp = fopen(tmp_object_path, "wb");
	if(!fp) {
		git_lfs_write_error(io, 400, "Failed to write to storage.");
		return;
	}

	while((n = io->read(io->context, buffer, sizeof(buffer))) > 0) {
		fwrite(buffer, 1, n, fp);
	}

	fclose(fp);

	if(config->verify_upload) {
		fp = fopen(tmp_object_path, "rb");
		if(fp) {
			SHA256_CTX ctx;
			SHA256_Init(&ctx);
			while((n = fread(buffer, 1, sizeof(buffer), fp)) > 0) {
				SHA256_Update(&ctx, buffer, n);
			}
			fclose(fp);
			unsigned char sha256[SHA256_DIGEST_LENGTH];
			unsigned char oid_hash[SHA256_DIGEST_LENGTH];
			SHA256_Final(sha256, &ctx);
			
			for(int i = 0; i < SHA256_DIGEST_LENGTH; i++) {
				oid_hash[i] = (hex_to_int(oid[i << 1]) << 4) | hex_to_int(oid[(i << 1) + 1]);
			}
			
			if(memcmp(oid_hash, sha256, SHA256_DIGEST_LENGTH) != 0) {
				git_lfs_write_error(400, "SHA256 written does not match the object SHA256: %s.", oid);
				return;
			}
		}
	}

	os_rename(tmp_object_path, object_path);

	io->write_http_status(io->context, 200, "OK");
	io->write_headers(io->context, NULL, 0);
}

static void git_lfs_verify(const struct git_lfs_config *config, const struct git_lfs_repo *repo, const struct socket_io *io)
{
	char buffer[4096];
	int n;
	json_tokener * tokener = json_tokener_new();
	struct json_object *root = NULL;

	while((n = io->read(io->context, buffer, sizeof(buffer))) > 0)
	{
		root = json_tokener_parse_ex(tokener, buffer, n);
		enum json_tokener_error err = json_tokener_get_error(tokener);
		if(err == json_tokener_success) break;
		if(err != json_tokener_continue) {
			git_lfs_write_error(io, 400, "JSON parsing error.");
			goto error0;
		}
	}
	
	struct json_object *oid, *size;
	if(!json_object_object_get_ex(root, "oid", &oid) ||
	   !json_object_is_type(oid, json_type_string) ||
	   !json_object_object_get_ex(root, "size", &size) ||
	   !json_object_is_type(size, json_type_int))
	{
		git_lfs_write_error(io, 400, "API error. Missing oid and size.");
		goto error0;
	}
	
	const char *oid_string = json_object_get_string(oid);
	if(!is_valid_oid(oid_string))
	{
		git_lfs_write_error(io, 400, "Invalid oid passed to verify.");
		goto error0;
	}

	char object_path[PATH_MAX];
	if(snprintf(object_path, sizeof(object_path), "%s/%.2s/%s", repo->root_dir, oid_string, oid_string) >= (long)sizeof(object_path))
	{
		git_lfs_write_error(io, 400, "Object path is too long.");
		goto error0;
	}
	
	long filesize = os_file_size(object_path);
	if(filesize < 0)
	{
		git_lfs_write_error(io, 404, "Object not found.");
		goto error0;
	}
	
	if(filesize != json_object_get_int(size))
	{
		git_lfs_write_error(io, 422, "Object size (%ld bytes) does not match the verify size (%d bytes).", filesize, json_object_get_int(size));
		goto error0;
	}
	
	const char *headers[] = {
		"Content-Type: application/vnd.git-lfs+json",
	};
	
	io->write_http_status(io->context, 200, "OK");
	io->write_headers(io->context, headers, sizeof(headers) / sizeof(headers[0]));

error0:
	if(root) json_object_put(root);
	json_tokener_free(tokener);
}

void git_lfs_server_handle_request(const struct git_lfs_config *config, const struct git_lfs_repo *repo, const struct socket_io *io, const char *method, const char *end_point)
{
	if(config->verbose >= 1)
	{
		time_t now;
		char currentTime[64];
		
		time(&now);
		
		strftime(currentTime, sizeof(currentTime), "%d/%b/%Y:%H:%M:%S %z", localtime(&now));

		printf("%s %s %s\n", currentTime, method, end_point);
	}

	if(strcmp(method, "GET") == 0)
	{
		if(strncmp(end_point, "/download/", 10) == 0) {
			git_lfs_download(config, repo, io, end_point + 10);
		} else {
			git_lfs_write_error(io, 501, "End point not supported.");
		}
		
	} else if(strcmp(method, "PUT") == 0) {
		
		if(strncmp(end_point, "/upload/", 8) == 0) {
			git_lfs_upload(config, repo, io, end_point + 8);
		} else {
			git_lfs_write_error(io, 501, "End point not supported.");
		}

	} else if(strcmp(method, "POST") == 0) {
		
		// v1 batch
		if(strcmp(end_point, "/objects/batch") == 0)
		{
			git_lfs_server_handle_batch(config, repo, io);
		} else if(strcmp(end_point, "/verify") == 0) {
			git_lfs_verify(config, repo, io);
		} else {
			git_lfs_write_error(io, 501, "End point not supported.");
		}
		
	} else {
		git_lfs_write_error(io, 501, "HTTP method not supported.");
	}
}
