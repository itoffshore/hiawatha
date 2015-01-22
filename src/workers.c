/* This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; version 2 of the License. For a copy,
 * see http://www.gnu.org/licenses/gpl-2.0.html.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 */

#include "config.h"
#include <sys/types.h>
#include <stdlib.h>
#include <stdbool.h>
#include <unistd.h>
#include <string.h>
#include <pthread.h>
#include <errno.h>
#include <sys/wait.h>
#include "libstr.h"
#include "liblist.h"
#include "http.h"
#include "session.h"
#include "client.h"
#include "target.h"
#include "envir.h"
#include "cgi.h"
#include "send.h"
#include "log.h"
#include "httpauth.h"
#include "tomahawk.h"
#include "ssl.h"
#include "cache.h"
#include "toolkit.h"
#include "xslt.h"
#include "monitor.h"
#include "memdbg.h"
#include "challenge.h"

extern char *hs_conlen;
char *fb_filesystem      = "access denied via filesystem";
char *fb_accesslist      = "access denied via accesslist";
char *unknown_host       = "(unknown)";
#ifdef ENABLE_CHALLENGE
volatile bool challenge_client_mode = false;
#endif

#ifdef ENABLE_THREAD_POOL
typedef struct type_thread_pool {
	pthread_t worker;
	t_session *session;
	bool quit;

	struct type_thread_pool *next;
} t_thread_pool;

typedef struct type_session_list {
	t_session *session;

	struct type_session_list *next;
} t_session_list;

static t_thread_pool *thread_pool = NULL;
static pthread_cond_t thread_pool_cond;
static pthread_mutex_t thread_pool_mutex;
static t_session_list *session_list = NULL;
static volatile int waiting_workers = 0;
static volatile int thread_pool_size = 0;
#endif

/* Send HTTP error message
 */
static int send_code(t_session *session) {
	int result;

	if ((result = send_http_code_header(session)) != 0) {
		return result;
	}

	if (((session->return_code >= 100) && (session->return_code < 200)) ||
	    (session->return_code == 204) || (session->return_code == 304)) {
		if (send_buffer(session, hs_conlen, 16) == -1) {
			return -1;
		}	
		return send_buffer(session, "0\r\n\r\n", 5);
	}

#ifdef ENABLE_XSLT
	if (session->host->error_xslt_file != NULL) {
		if (show_http_code_body(session) == 0) {
			return 0;
		}
	}
#endif

	return send_http_code_body(session);
}

/* Check if the requested file is a CGI program.
 */
static t_cgi_type check_target_is_cgi(t_session *session) {
	t_cgi_handler *cgi;

	session->cgi_handler = NULL;
#ifdef ENABLE_TOOLKIT
	if ((session->fcgi_server = find_fcgi_server(session->config->fcgi_server, session->toolkit_fastcgi)) != NULL) {
		session->cgi_type = fastcgi;
		session->host->execute_cgi = true;
	} else
#endif
	if ((session->fcgi_server = fcgi_server_match(session->config->fcgi_server, &(session->host->fast_cgi), session->extension)) != NULL) {
		session->cgi_type = fastcgi;
	} else if (in_charlist(session->extension, &(session->config->cgi_extension))) {
		session->cgi_type = binary;
	} else {
		session->cgi_type = no_cgi;
		cgi = session->config->cgi_handler;
		while (cgi != NULL) {
			if (in_charlist(session->extension, &(cgi->extension))) {
				session->cgi_handler = cgi->handler;
				session->cgi_type = script;
				break;
			}
			cgi = cgi->next;
		}
	}

	return session->cgi_type;
}

/* Handle an HTTP error.
 */
static int handle_error(t_session *session, int error_code) {
	t_error_handler *error_handler;
	char *new_fod;
	int result = -1;
#ifdef ENABLE_XSLT
	char *xslt_file;
#endif

	error_handler = session->host->error_handlers;
	while (error_handler != NULL) {
		if (error_handler->code == error_code) {
			break;
		}
		error_handler = error_handler->next;
	}

	if (error_handler == NULL) {
		return 0;
	}

	session->return_code = error_code;
	session->error_code = error_code;
	session->handling_error = true;
	session->mimetype = NULL;
	session->vars = error_handler->parameters;

	if ((new_fod = (char*)malloc(session->host->website_root_len + strlen(error_handler->handler) + 4)) == NULL) { /* + 3 for .gz (gzip encoding) */
		log_error(session, "malloc() error while handling error");
		return 500;
	}

	if (session->file_on_disk != NULL) {
		free(session->file_on_disk);
	}
	session->file_on_disk = new_fod;

	memcpy(session->file_on_disk, session->host->website_root, session->host->website_root_len);
	strcpy(session->file_on_disk + session->host->website_root_len, error_handler->handler);

	if (get_target_extension(session) == -1) {
		log_error(session, "error getting extension while handing error");
		return 500;
	}
	check_target_is_cgi(session);

	if (session->cgi_type != no_cgi) {
		result = execute_cgi(session);
#ifdef ENABLE_XSLT
	} else if ((xslt_file = find_xslt_file(session)) != NULL) {
		result = handle_xml_file(session, xslt_file);
		free(xslt_file);
#endif
	} else switch (is_directory(session->file_on_disk)) {
		case error:
			result = 500;
			break;
		case yes:
			result = 301;
			break;
		case no:
			result = send_file(session);
			break;
		case no_access:
			result = 403;
			break;
		case not_found:
			result = 404;
			break;
	}

	switch (result) {
		case 301:
			log_error(session, "ErrorHandler is a directory");
			break;
		case 403:
			log_error(session, "no access to ErrorHandler");
			break;
		case 404:
			log_error(session, "ErrorHandler not found");
			break;
		case 500:
			log_file_error(session, error_handler->handler, "internal error for ErrorHandler");
			session->keep_alive = false;
			break;
		case 503:
			log_file_error(session, error_handler->handler, "FastCGI for ErrorHandler not available");
			break;
	}

	return result;
}

/* Run a program
 */
static int run_program(t_session *session, char *program, int return_code) {
	pid_t pid;
	char ip[MAX_IP_STR_LEN], value[10], *pos, slash = '/';

	switch (pid = fork()) {
		case -1:
			log_file_error(session, program, "fork() error");
			return -1;
		case 0:
			if (setsid() == -1) {
				log_file_error(session, program, "setsid() error");
			} else {
				/* Close all other open filedescriptors.
				 */
				close_bindings(session->config->binding);
				close_client_sockets_for_cgi_run();
				close_logfiles_for_cgi_run(session->config->first_host);

				/* Set environment variables
				 */
				setenv("REQUEST_METHOD", session->method, 1);
				setenv("DOCUMENT_ROOT", session->host->website_root, 1);
				setenv("REQUEST_URI", session->request_uri, 1);
				if (session->remote_user != NULL) {
					setenv("REMOTE_USER", session->remote_user, 1);
				}
				if (inet_ntop(session->ip_address.family, &(session->ip_address.value), ip, MAX_IP_STR_LEN) != NULL) {
					setenv("REMOTE_ADDR", ip, 1);
				}
				snprintf(value, 9, "%d", return_code);
				setenv("HTTP_RETURN_CODE", value, 1);

				http_header_to_environment(session, NULL, "Range:", "HTTP_RANGE");
				http_header_to_environment(session, NULL, "Referer:", "HTTP_REFERER");
				http_header_to_environment(session, NULL, "User-Agent:", "HTTP_USER_AGENT");

				/* Change directory to program's directory
				 */
				pos = strrchr(program, slash);
#ifdef CYGWIN
				if ((pos == NULL) && (session->config->platform == windows)) {
					slash = '\\';
					pos = strrchr(program, slash);
				}
#endif
				if (pos != NULL) {
					*pos = '\0';
					if (chdir(program) == -1) {
						exit(EXIT_FAILURE);
					}
					*pos = slash;
				}

				/* Execute program
				 */
				execlp(program, program, (char*)NULL);
				log_file_error(session, program, "exec() error");
			}
			exit(EXIT_FAILURE);
		default:
			if (session->config->wait_for_cgi) {
				waitpid(pid, NULL, 0);
			}
	}

	return 0;
}

static t_access allow_client(t_session *session) {
	t_ip_addr forwarded_ip;
	t_access access;

	if ((access = ip_allowed(&(session->ip_address), session->host->access_list)) != allow) {
		return access;
 	} else if (last_forwarded_ip(session->http_headers, &forwarded_ip) == -1) {
		return allow;
	} else if (ip_allowed(&forwarded_ip, session->host->access_list) == deny) {
		return deny;
	}

	return unspecified;
}

/* Serve the client that connected to the webserver
 */
static int serve_client(t_session *session) {
	int result, length, auth_result, connections_per_ip, total_connections = -1;
	char *qmark, chr, *header;
	t_host *host_record;
	t_access access;
	t_deny_body *deny_body;
	t_req_method request_method;
	t_ip_addr ip_addr;
#ifdef ENABLE_XSLT
	char *xslt_file;
#endif
#ifdef ENABLE_TOOLKIT
	int i;
	t_toolkit_options toolkit_options;
#endif
#ifdef ENABLE_RPROXY
	t_rproxy *rproxy = NULL;
#endif

#ifdef ENABLE_DEBUG
	session->current_task = "fetch & parse request";
#endif

	if ((result = fetch_request(session)) != 200) {
		session->request_method = GET;
		return result;
	} else if ((result = parse_request(session, session->header_length + session->content_length)) != 200) {
		session->request_method = GET;
		return result;
	}

#ifdef ENABLE_DEBUG
	session->current_task = "serve client";
#endif

	session->time = time(NULL);

	/* Hide reverse proxies
	 */
	if (session->via_trusted_proxy) {
		if (last_forwarded_ip(session->http_headers, &ip_addr) == 0) {
			if (reposition_client(session, &ip_addr) != -1) {
				copy_ip(&(session->ip_address), &ip_addr);

				if (session->request_limit == false) {
					connections_per_ip = session->config->total_connections;
				} else {
					connections_per_ip = session->config->connections_per_ip;
				}

				if ((total_connections = connection_allowed(&ip_addr, false, connections_per_ip, session->config->total_connections)) < 0) {
					session->keep_alive = false;
					return handle_connection_not_allowed(session, total_connections);
				}
			}
		}
	}

#ifdef ENABLE_RPROXY
	/* SSH tunneling
	 */
	if (session->request_method == CONNECT) {
		if (in_iplist(session->config->tunnel_ssh, &(session->ip_address)) == false) {
			return 405;
		}

#ifdef ENABLE_SSL
		if (session->binding->use_ssl) {
			return 405;
		}
#endif

		if (strcmp(session->request_uri, "localhost:22") != 0) {
			if (strcmp(session->request_uri, "127.0.0.1:22") != 0) {
				if (strcmp(session->request_uri, "::1.22") != 0) {
					return 403;
				}
			}
		}

		log_system(session, "SSH tunnel requested");
		if (tunnel_ssh_connection(session->client_socket) != 0) {
			log_system(session, "SSH tunnel failed");
		} else {
			log_system(session, "SSH tunnel terminated");
		}

		session->keep_alive = false;

		return 200;
	}
#endif

#ifdef ENABLE_CHALLENGE
	/* Challenge client
	 */
	if (session->config->challenge_threshold >= 0) {
		if (ip_allowed(&(session->ip_address), session->config->banlist_mask) != deny) {
			if (total_connections == -1) {
				total_connections = count_registered_connections();
			}

			if (challenge_client_mode == false) {
				if (total_connections >= session->config->challenge_threshold) {
					challenge_client_mode = true;
					log_system(session, "ChallengeClient mode activated");
				}
			} else {
				if (total_connections < 0.9 * session->config->challenge_threshold) {
					challenge_client_mode = false;
					log_system(session, "ChallengeClient mode deactivated");
				}
			}

			if (challenge_client_mode) {
				if ((result = challenge_client(session)) != 0) {
					if ((result != 200) && (session->config->challenge_ban > 0)) {
						ban_ip(&(session->ip_address), session->config->challenge_ban, session->config->kick_on_ban);
						log_system(session, "Client banned due to challenge failure");
#ifdef ENABLE_MONITOR
						if (session->config->monitor_enabled) {
							monitor_count_ban(session);
						}
#endif
					} else if (result == 403) {
						session->keep_alive = false;
					}

					return result;
				}
			}
		}
	}
#endif

	/* Find host record
	 */
	if (session->hostname != NULL) {
		if (remove_port_from_hostname(session) == -1) {
			log_error(session, "error removing port from hostname");
			return 500;
		}

		if ((host_record = get_hostrecord(session->config->first_host, session->hostname, session->binding)) != NULL) {
			session->host = host_record;
#ifdef ENABLE_TOMAHAWK
			session->last_host = host_record;
#endif
		}
	}
	session->host->access_time = session->time;

#ifdef ENABLE_SSL
	/* SSL client authentication
	 */
	if (session->binding->use_ssl) {
		if ((session->host->ca_certificate != NULL) && (ssl_has_peer_cert(&(session->ssl_context)) == false)) {
			log_error(session, "Missing client SSL certificate");
			return 440;
		}
	}
#endif

	/* Enforce usage of first hostname
	 */
	if (session->host->enforce_first_hostname && (session->hostname != NULL)) {
		if (**(session->host->hostname.item) != '*') {
			if (strcmp(session->hostname, *(session->host->hostname.item)) != 0) {
				session->cause_of_301 = enforce_first_hostname;
				return 301;
			}
		}
	}

	/* Enforce usage of SSL
	 */
#ifdef ENABLE_SSL
	if (session->host->require_ssl && (session->binding->use_ssl == false)) {
		if ((qmark = strchr(session->uri, '?')) != NULL) {
			*qmark = '\0';
			session->vars = qmark + 1;
			session->uri_len = strlen(session->uri);
		}
		session->cause_of_301 = require_ssl;
		return 301;
	}
#endif

	/* Deny matching bodies
	 */
	if (session->body != NULL) {
		chr = *(session->body + session->content_length);
		*(session->body + session->content_length) = '\0';

		deny_body = session->host->deny_body;
		while (deny_body != NULL) {
			if (strpcmp(session->body, &(deny_body->pattern)) == 0) {
				if ((session->config->ban_on_denied_body > 0) && (ip_allowed(&(session->ip_address), session->config->banlist_mask) != deny)) {
					ban_ip(&(session->ip_address), session->config->ban_on_denied_body, session->config->kick_on_ban);
					log_system(session, "Client banned because of denied body");
					session->keep_alive = false;
#ifdef ENABLE_MONITOR
					if (session->config->monitor_enabled) {
						monitor_count_ban(session);
					}
#endif
				}

				log_exploit_attempt(session, "denied body", session->body);
#ifdef ENABLE_TOMAHAWK
				increment_counter(COUNTER_EXPLOIT);
#endif
#ifdef ENABLE_MONITOR
				if (session->config->monitor_enabled) {
					monitor_count_exploit(session);
					monitor_event("Request body denied for %s", session->host->hostname.item[0]);
				}
#endif

				*(session->body + session->content_length) = chr;

				return 403;
			}
			deny_body = deny_body->next;
		}

		*(session->body + session->content_length) = chr;
	}

	/* Websocket
	 */
	if (session->request_method == GET) {
		if ((header = get_http_header("Connection:", session->http_headers)) != NULL) {
			if (strcasestr(header, "upgrade") != NULL) {
				if ((header = get_http_header("Upgrade:", session->http_headers)) != NULL) {
					if (strcasecmp(header, "websocket") == 0) {
						switch (access = allow_client(session)) {
							case deny:
								log_error(session, fb_accesslist);
								return 403;
							case allow:
								break;
							case pwd:
							case unspecified:
								if ((auth_result = http_authentication_result(session, access == unspecified)) != 200) {
									return auth_result;
								}
						}

						session->keep_alive = false;
						if (forward_to_websocket(session) == -1) {
							return 500;
						}

						return 200;
					}
				}
			}
		}
	}

#ifdef ENABLE_RPROXY
	rproxy = find_rproxy(session->host->rproxy, session->request_uri
#ifdef ENABLE_SSL
		, session->binding->use_ssl
#endif
		);

	/* Actions based on request method
	 */
	if (rproxy == NULL)
#endif
	switch (session->request_method) {
		case TRACE:
			if (session->binding->enable_trace == false) {
				return 501;
			}
			return handle_trace_request(session);
		case PUT:
		case DELETE:
			if ((session->binding->enable_alter == false) && (session->host->webdav_app == false)) {
				return 501;
			}
			break;
		case unknown:
			return 400;
		case unsupported:
			if (session->host->webdav_app == false) {
				return 501;
			}
			break;
		default:
			break;
	}

	if (duplicate_host(session) == false) {
		log_error(session, "duplicate_host() error");
		return 500;
	}

#ifdef ENABLE_TOOLKIT
	if (session->host->ignore_dot_hiawatha == false) {
		if (load_user_root_config(session) == -1) {
			return 500;
		}
	}

	if (total_connections == -1) {
		total_connections = count_registered_connections();
	}

	/* URL toolkit
	 */
	init_toolkit_options(&toolkit_options);
	toolkit_options.method = session->method;
	toolkit_options.website_root = session->host->website_root;
	toolkit_options.url_toolkit = session->config->url_toolkit;
	toolkit_options.allow_dot_files = session->host->allow_dot_files;
	toolkit_options.http_headers = session->http_headers;
	toolkit_options.total_connections = total_connections;
	toolkit_options.log_request = true;
#ifdef ENABLE_SSL
	toolkit_options.use_ssl = session->binding->use_ssl;
#endif

	for (i = 0; i < session->host->toolkit_rules.size; i++) {
		if ((result = use_toolkit(session->uri, session->host->toolkit_rules.item[i], &toolkit_options)) == UT_ERROR) {
			return 500;
		}

		if (toolkit_options.log_request == false) {
			session->log_request = false;
		}

		if ((toolkit_options.ban > 0) && (ip_allowed(&(session->ip_address), session->config->banlist_mask) != deny)) {
			ban_ip(&(session->ip_address), toolkit_options.ban, session->config->kick_on_ban);
			log_system(session, "Client banned because of URL match in UrlToolkit rule");
			session->keep_alive = false;
#ifdef ENABLE_MONITOR
			if (session->config->monitor_enabled) {
				monitor_count_ban(session);
			}
#endif
			return 403;
		}

		session->toolkit_fastcgi = toolkit_options.fastcgi_server;

		if (toolkit_options.new_url != NULL) {
			if (register_tempdata(&(session->tempdata), toolkit_options.new_url, tc_data) == -1) {
				free(toolkit_options.new_url);
				log_error(session, "error registering temporary data");
				return 500;
			}
			session->uri = toolkit_options.new_url;
		}

		if (result == UT_REDIRECT) {
			if ((session->location = strdup(toolkit_options.new_url)) == NULL) {
				return -1;
			}
			session->cause_of_301 = location;
			return 301;
		}

		if (result == UT_DENY_ACCESS) {
			log_error(session, "access denied via URL toolkit rule");
			return 403;
		}

		if (toolkit_options.expire > -1) {
			session->expires = toolkit_options.expire;
			session->caco_private = toolkit_options.caco_private;
		}
	}
#endif

	/* Find GET data
	 */
	if ((qmark = strchr(session->uri, '?')) != NULL) {
		*qmark = '\0';
		session->vars = qmark + 1;
	}

	url_decode(session->uri);
	session->uri_len = strlen(session->uri);

	if ((session->vars != NULL) && (session->host->secure_url)) {
		if (forbidden_chars_present(session->vars)) {
			log_error(session, "URL contains forbidden characters");
			return 403;
		}
	}

	if (validate_url(session) == false) {
		return -1;
	}

#ifdef ENABLE_RPROXY
	/* Reverse proxy
	 */
	if (rproxy != NULL) {
		if (rproxy_loop_detected(session->http_headers)) {
			return 508;
		}

		switch (access = allow_client(session)) {
			case deny:
				log_error(session, fb_accesslist);
				return 403;
			case allow:
				break;
			case pwd:
			case unspecified:
				if ((auth_result = http_authentication_result(session, access == unspecified)) != 200) {
					return auth_result;
				}
		}

		/* Prevent SQL injection
		 */
		if (session->host->prevent_sqli) {
			result = prevent_sqli(session);
			if (result == 1) {
				session->error_cause = ec_SQL_INJECTION;
			}
			if (result != 0) {
				return -1;
			}
		}

		/* Prevent Cross-site Scripting
		 */
		if (session->host->prevent_xss != p_no) {
			if (prevent_xss(session) > 0) {
				if (session->host->prevent_xss == p_block) {
					session->error_cause = ec_XSS;
					return -1;
				}
			}
		}

		/* Prevent Cross-site Request Forgery
		 */
		if (session->host->prevent_csrf != p_no) {
			if (prevent_csrf(session) > 0) {
				if (session->host->prevent_csrf == p_block) {
					session->error_cause = ec_CSRF;
					return -1;
				}
			}
		}

		return proxy_request(session, rproxy);
	}
#endif

	if ((result = uri_to_path(session)) != 200) {
		return result;
	}

	/* Load configfile from directories
	 */
	if (session->host->ignore_dot_hiawatha == false) {
		if (load_user_config(session) == -1) {
			return 500;
		}
	}

	if ((result = copy_directory_settings(session)) != 200) {
		return result;
	}

	switch (access = allow_client(session)) {
		case deny:
			log_error(session, fb_accesslist);
			return 403;
		case allow:
			break;
		case pwd:
		case unspecified:
			if ((auth_result = http_authentication_result(session, access == unspecified)) != 200) {
				return auth_result;
			}
	}

	switch (is_directory(session->file_on_disk)) {
		case error:
			return 500;
		case yes:
			session->uri_is_dir = true;
			break;
		case no:
			if (((session->request_method != PUT) || session->host->webdav_app) && (session->host->enable_path_info)) {
				if ((result = get_path_info(session)) != 200) {
					return result;
				}
			}
			break;
		case no_access:
			log_error(session, fb_filesystem);
			return 403;
		case not_found:
			if (session->request_method == DELETE) {
				return 404;
			}
	}

#ifdef ENABLE_TOOLKIT
	if ((session->toolkit_fastcgi == NULL) && session->uri_is_dir) {
#else
	if (session->uri_is_dir) {
#endif
		length = strlen(session->file_on_disk);
		if (*(session->file_on_disk + length - 1) == '/') {
			strcpy(session->file_on_disk + length, session->host->start_file);
		} else {
			return 301;
		}
	}

	if (get_target_extension(session) == -1) {
		log_error(session, "error getting extension");
		return 500;
	}

	if (((session->request_method != PUT) && (session->request_method != DELETE)) || session->host->webdav_app) {
		check_target_is_cgi(session);
	}

	/* Handle request based on request method
	 */
	request_method = session->request_method;
	if (session->host->webdav_app) {
		if ((request_method == PUT) || (request_method == DELETE)) {
			request_method = POST;
		}
	}

	switch (request_method) {
		case GET:
		case HEAD:
			if (session->cgi_type != no_cgi) {
				session->body = NULL;
				result = execute_cgi(session);
#ifdef ENABLE_XSLT
			} else if ((xslt_file = find_xslt_file(session)) != NULL) {
				result = handle_xml_file(session, xslt_file);
				free(xslt_file);
#endif
			} else {
				result = send_file(session);
			}
			if (result == 404) {
#ifdef ENABLE_XSLT
				if ((session->host->show_index != NULL) && (session->uri[session->uri_len - 1] == '/')) {
					result = show_index(session);
				}
#endif
#ifdef ENABLE_MONITOR
			} else if (session->config->monitor_enabled) {
				if ((result == 200) && (session->host->monitor_host)) {
					unlink(session->file_on_disk);
				}
#endif
			}

			if ((session->request_method == GET) && (session->cgi_type == no_cgi) && (session->directory != NULL)) {
				if (session->directory->run_on_download != NULL) {
					run_program(session, session->directory->run_on_download, result);
				}
			}
			break;
		case POST:
		case unsupported:
			if (session->cgi_type != no_cgi) {
				result = execute_cgi(session);
#ifdef ENABLE_XSLT
			} else if ((xslt_file = find_xslt_file(session)) != NULL) {
				result = handle_xml_file(session, xslt_file);
				free(xslt_file);
#endif
			} else {
				result = 405;
			}
			break;
		case PUT:
			result = handle_put_request(session);
			if (((result == 201) || (result == 204)) && (session->host->run_on_alter != NULL)) {
				run_program(session, session->host->run_on_alter, result);
			}
			break;
		case DELETE:
			result = handle_delete_request(session);
			if ((result == 204) && (session->host->run_on_alter != NULL)) {
				run_program(session, session->host->run_on_alter, result);
			}
			break;
		case WHEN:
			send_code(session);
			break;
		default:
			result = 400;
	}

	return result;
}

/* Handle timeout upon sending request
 */
static void handle_timeout(t_session *session) {
	if ((session->config->ban_on_timeout > 0) && (ip_allowed(&(session->ip_address), session->config->banlist_mask) != deny)) {
		ban_ip(&(session->ip_address), session->config->ban_on_timeout, session->config->kick_on_ban);
		log_system(session, "Client banned because of connection timeout");
		session->keep_alive = false;
#ifdef ENABLE_MONITOR
		if (session->config->monitor_enabled) {
			monitor_count_ban(session);
		}
#endif
	} else {
		log_system(session, "Timeout while waiting for first request");
	}
}

/* Request has been handled, handle the return code.
 */
static void handle_request_result(t_session *session, int result) {
	char *hostname;

#ifdef ENABLE_DEBUG
	session->current_task = "handle request result";
#endif

	if (result == -1) switch (session->error_cause) {
		case ec_MAX_REQUESTSIZE:
			log_system(session, "Maximum request size reached");
			session->return_code = 413;
			send_code(session);
			if ((session->config->ban_on_max_request_size > 0) && (ip_allowed(&(session->ip_address), session->config->banlist_mask) != deny)) {
				ban_ip(&(session->ip_address), session->config->ban_on_max_request_size, session->config->kick_on_ban);
				log_system(session, "Client banned because of sending a too large request");
				session->keep_alive = false;
#ifdef ENABLE_MONITOR
				if (session->config->monitor_enabled) {
					monitor_count_ban(session);
				}
#endif
			}
			break;
		case ec_TIMEOUT:
			if (session->kept_alive == 0) {
				session->return_code = 408;
				send_code(session);
				handle_timeout(session);
			}
			break;
		case ec_CLIENT_DISCONNECTED:
			if (session->kept_alive == 0) {
				log_system(session, "Silent client disconnected");
			}
			break;
		case ec_SOCKET_READ_ERROR:
			if (errno != ECONNRESET) {
				log_system(session, "Error while reading request");
			}
			break;
		case ec_SOCKET_WRITE_ERROR:
			if (session->log_request) {
				log_request(session);
			}
			break;
		case ec_FORCE_QUIT:
			log_system(session, "Client kicked");
			break;
		case ec_SQL_INJECTION:
			if ((session->config->ban_on_sqli > 0) && (ip_allowed(&(session->ip_address), session->config->banlist_mask) != deny)) {
				ban_ip(&(session->ip_address), session->config->ban_on_sqli, session->config->kick_on_ban);
				hostname = (session->hostname != NULL) ? session->hostname : unknown_host;
				log_system(session, "Client banned because of SQL injection on %s", hostname);
				session->keep_alive = false;
#ifdef ENABLE_MONITOR
				if (session->config->monitor_enabled) {
					monitor_count_ban(session);
				}
#endif
			}
			//session->return_code = 441;
			session->return_code = 404;
			send_code(session);
			if (session->log_request) {
				log_request(session);
			}
			break;
		case ec_XSS:
			session->return_code = 442;
			send_code(session);
			if (session->log_request) {
				log_request(session);
			}
			break;
		case ec_CSRF:
			session->return_code = 443;
			send_code(session);
			if (session->log_request) {
				log_request(session);
			}
			break;
		case ec_INVALID_URL:
			if ((session->config->ban_on_invalid_url > 0) && (ip_allowed(&(session->ip_address), session->config->banlist_mask) != deny)) {
				ban_ip(&(session->ip_address), session->config->ban_on_invalid_url, session->config->kick_on_ban);
				hostname = (session->hostname != NULL) ? session->hostname : unknown_host;
				log_system(session, "Client banned because of invalid URL on %s", hostname);
				session->keep_alive = false;
#ifdef ENABLE_MONITOR
				if (session->config->monitor_enabled) {
					monitor_count_ban(session);
				}
#endif
			}
			send_code(session);
			break;
		default:
			if (session->data_sent == false) {
				session->return_code = 500;
				if (send_code(session) == -1) {
					session->keep_alive = false;
				}
			}
	} else switch (result) {
		case 200:
			break;
		case 201:
		case 204:
		case 304:
		case 412:
			if (session->data_sent == false) {
				session->return_code = result;
				if (send_header(session) == -1) {
					session->keep_alive = false;
				} else if (send_buffer(session, "Content-Length: 0\r\n\r\n", 21) == -1) {
					session->keep_alive = false;
				}
			}
			break;
		case 411:
		case 413:
			session->keep_alive = false;
			if (session->data_sent == false) {
				session->return_code = result;
				if (send_header(session) == -1) {
					session->keep_alive = false;
				} else if (send_buffer(session, "Content-Length: 0\r\n\r\n", 21) == -1) {
					session->keep_alive = false;
				}
			}
			break;
		case 400:
			log_garbage(session);
			if (session->data_sent == false) {
				session->return_code = 400;
				if (send_code(session) == -1) {
					session->keep_alive = false;
				}
			}
			if ((session->config->ban_on_garbage > 0) && (ip_allowed(&(session->ip_address), session->config->banlist_mask) != deny)) {
				ban_ip(&(session->ip_address), session->config->ban_on_garbage, session->config->kick_on_ban);
				log_system(session, "Client banned because of sending garbage");
				session->keep_alive = false;
#ifdef ENABLE_MONITOR
				if (session->config->monitor_enabled) {
					monitor_count_ban(session);
				}
#endif
			}
#ifdef ENABLE_MONITOR
			if (session->config->monitor_enabled) {
				monitor_count_bad_request(session);
			}
#endif
			break;
		case 401:
		case 403:
		case 404:
		case 501:
		case 503:
			if (session->data_sent == false) {
				switch (handle_error(session, result)) {
					case -1:
						session->keep_alive = false;
						break;
					case 200:
						break;
					default:
						if (session->data_sent == false) {
							session->return_code = result;
							if (send_code(session) == -1) {
								session->keep_alive = false;
							}
						}
				}
			}
			break;
		case 500:
			session->keep_alive = false;
		default:
			if (session->data_sent == false) {
				session->return_code = result;
				send_code(session);
			}
	}

	if ((result > 0) && (result != 400)) {
		if (session->log_request) {
			log_request(session);
		}
	} else {
		session->keep_alive = false;
	}
}

/* Handle the connection of a client.
 */
static void connection_handler(t_session *session) {
	int result;
#ifdef ENABLE_SSL
	t_ssl_accept_data sad;
#endif

#ifdef ENABLE_DEBUG
	session->current_task = "thread started";
#endif

#ifdef ENABLE_SSL
	if (session->binding->use_ssl) {
		sad.context         = &(session->ssl_context);
		sad.client_fd       = &(session->client_socket);
		sad.private_key     = session->binding->private_key;
		sad.certificate     = session->binding->certificate;
		sad.ca_certificate  = session->binding->ca_certificate;
		sad.ca_crl          = session->binding->ca_crl;

		sad.timeout         = session->kept_alive == 0 ? session->binding->time_for_1st_request : session->binding->time_for_request;
		sad.min_ssl_version = session->config->min_ssl_version;
		sad.dh_size         = session->config->dh_size;
#ifdef ENABLE_DEBUG
		sad.thread_id       = session->thread_id;
		session->current_task = "ssl accept";
#endif
		switch (ssl_accept(&sad)) {
			case SSL_HANDSHAKE_NO_MATCH:
				log_system(session, "No cypher overlap during SSL handshake.");
				break;
			case SSL_HANDSHAKE_TIMEOUT:
				handle_timeout(session);
				break;
			case SSL_HANDSHAKE_OKE:
				session->socket_open = true;
				break;
		}
	} else
#endif
		session->socket_open = true;

	if (session->socket_open) {
#ifdef ENABLE_MONITOR
		if (session->config->monitor_enabled) {
			monitor_count_connection(session);
		}
#endif

		do {
			result = serve_client(session);
			handle_request_result(session, result);
#ifdef ENABLE_TOMAHAWK
			if (session->parsing_oke) {
				show_request_to_admins(session->method, session->request_uri, session->http_version, &(session->ip_address),
				                       session->http_headers, session->return_code, session->bytes_sent);
			}
#endif

#ifdef ENABLE_DEBUG
			session->current_task = "request done";
#endif

			if (session->socket_open) {
				/* Flush the output-buffer
				 */
				if (send_buffer(session, NULL, 0) == -1) {
					session->keep_alive = false;
				}
			}

#ifdef ENABLE_MONITOR
			if (session->config->monitor_enabled) {
				monitor_count_host(session);
			}
#endif
			reset_session(session);
#ifdef ENABLE_DEBUG
			session->current_task = "session reset";
#endif

			if ((session->kept_alive > 0) && (session->config->ban_on_flooding > 0)) {
				if (client_is_flooding(session)) {
					if (ip_allowed(&(session->ip_address), session->config->banlist_mask) != deny) {
						ban_ip(&(session->ip_address), session->config->ban_on_flooding, session->config->kick_on_ban);
						log_system(session, "Client banned because of flooding");
						session->keep_alive = false;
#ifdef ENABLE_MONITOR
						if (session->config->monitor_enabled) {
							monitor_count_ban(session);
						}
#endif
					}
				}
			}
		} while (session->keep_alive && session->socket_open);
#ifdef ENABLE_DEBUG
		session->current_task = "session done";
#endif

		destroy_session(session);
		close_socket(session);
	} else {
		close(session->client_socket);
	}

	if (session->config->reconnect_delay > 0) {
		mark_client_for_removal(session, session->config->reconnect_delay);
	} else {
		remove_client(session, true);
	}

#ifdef ENABLE_DEBUG
	/* Show memory usage by thread
	 */
	memdbg_print_log(false);
#endif

	/* Client session ends here
	 */
#ifndef ENABLE_THREAD_POOL
	pthread_exit(NULL);
#endif
}

#ifdef ENABLE_THREAD_POOL
/* Get last session record from session list
 */
static t_session *last_session(t_session_list *list) {
	t_session_list *prev = NULL;
	t_session *session;

	if (list == NULL) {
		return NULL;
	}

	while (list->next != NULL) {
		prev = list;
		list = list->next;
	}

	if (prev == NULL) {
		session_list = NULL;
	} else {
		prev->next = NULL;
	}

	session = list->session;
	free(list);

	return session;
}

/* Main loop for thread in thread pool
 */
static void thread_wait_loop(t_thread_pool *self) {
	t_session *session;
	t_thread_pool *prev;

	do {
		if (self->session == NULL) {
			pthread_mutex_lock(&thread_pool_mutex);

			waiting_workers++;
			if (pthread_cond_wait(&thread_pool_cond, &thread_pool_mutex) != 0) {
				waiting_workers--;
				session = NULL;
			} else {
				session = last_session(session_list);
			}

			pthread_mutex_unlock(&thread_pool_mutex);
		} else {
			session = self->session;
			self->session = NULL;
		}

		if (session != NULL) {
			if (add_client(session) == 0) {
				connection_handler(session);
			} else {
				close(session->client_socket);
				free(session);
			}
		}
	} while (self->quit == false);

	/* Remove thread record from pool
	 */
	pthread_mutex_lock(&thread_pool_mutex);

	if (thread_pool == self) {
		thread_pool = thread_pool->next;
	} else {
		prev = thread_pool;
		while (prev != NULL) {
			if (prev->next == self) {
				prev->next = self->next;
				break;
			}
			prev = prev->next;
		}
	}
	thread_pool_size--;

	pthread_mutex_unlock(&thread_pool_mutex);

	free(self);

	pthread_exit(NULL);
}

/* Add thread to thread pool
 */
static int add_thread_to_pool(t_session *session) {
	t_thread_pool *new_thread;
	pthread_attr_t child_attr;

	if ((new_thread = (t_thread_pool*)malloc(sizeof(t_thread_pool))) == NULL) {
		return -1;
	}

	new_thread->session = session;
	new_thread->quit = false;

	if (pthread_attr_init(&child_attr) != 0) {
		fprintf(stderr, "pthread init error.\n");
		free(new_thread);
		return -1;
	} else if (pthread_attr_setdetachstate(&child_attr, PTHREAD_CREATE_DETACHED) != 0) {
		fprintf(stderr, "pthread set detach state error.\n");
		pthread_attr_destroy(&child_attr);
		free(new_thread);
		return -1;
	} else if (pthread_attr_setstacksize(&child_attr, PTHREAD_STACK_SIZE) != 0) {
		fprintf(stderr, "pthread set stack size error.\n");
		pthread_attr_destroy(&child_attr);
		free(new_thread);
		return -1;
	} else if (pthread_create(&(new_thread->worker), &child_attr, (void*)thread_wait_loop, (void*)new_thread) != 0) {
		perror("pthread create error");
		pthread_attr_destroy(&child_attr);
		free(new_thread);
		return -1;
	}

	pthread_attr_destroy(&child_attr);

	new_thread->next = thread_pool;
	thread_pool = new_thread;
	thread_pool_size++;

	return 0;
}
#endif

/* Start worker
 */
int start_worker(t_session *session) {
#ifndef ENABLE_THREAD_POOL
	int result = -1;
	pthread_attr_t child_attr;
	pthread_t      child_thread;

	if (pthread_attr_init(&child_attr) != 0) {
		log_system(session, "pthread init error");
	} else {
		if (pthread_attr_setdetachstate(&child_attr, PTHREAD_CREATE_DETACHED) != 0) {
			log_system(session, "pthread set detach state error");
		} else if (pthread_attr_setstacksize(&child_attr, PTHREAD_STACK_SIZE) != 0) {
			log_system(session, "pthread set stack size error");
		} else if (add_client(session) == 0) {
			if (pthread_create(&child_thread, &child_attr, (void*)connection_handler, (void*)session) == 0) {
				/* Worker thread started
				 */
				result = 0;
			} else {
				remove_client(session, false);
				log_system(session, "pthread create error");
			}
		}
		pthread_attr_destroy(&child_attr);
	}

	return result;
#else
	int result = 0;
	t_session_list *new_session;

	pthread_mutex_lock(&thread_pool_mutex);

	if (waiting_workers <= 0) {
		if (add_thread_to_pool(session) == -1) {
			result = -1;
		}
	} else if ((new_session = (t_session_list*)malloc(sizeof(t_session_list))) == NULL) {
		result = -1;
	} else {
		new_session->session = session;

		new_session->next = session_list;
		session_list = new_session;

		if (pthread_cond_signal(&thread_pool_cond) != 0) {
			result = -1;
			session_list = session_list->next;
			free(new_session);
		} else {
			waiting_workers--;
		}
	}

	pthread_mutex_unlock(&thread_pool_mutex);

	return result;
#endif
}

/* Initialize worker module
 */
#ifdef ENABLE_THREAD_POOL
int init_workers_module(int pool_size) {
	int i;

	if (pthread_cond_init(&thread_pool_cond, NULL) != 0) {
		return -1;
	} else if (pthread_mutex_init(&thread_pool_mutex, NULL) != 0) {
		return -1;
	}

	for (i = 0; i < pool_size; i++) {
		if (add_thread_to_pool(NULL) == -1) {
			return -1;
		}
	}

	return 0;
}

/* Check thread pool
 */
void manage_thread_pool(int default_thread_pool_size, int thread_kill_rate) {
	int last_run = 0, kill;
	t_thread_pool *thread;

	pthread_mutex_lock(&thread_pool_mutex);

	thread = thread_pool;
	while (thread != NULL) {
		if (thread->quit) {
			last_run++;
		}
		thread = thread->next;
	}

	kill = (thread_pool_size - last_run) - default_thread_pool_size;

	if (kill > 0) {
		if (kill > thread_kill_rate) {
			kill = thread_kill_rate;
		}

		thread = thread_pool;
		while (thread != NULL) {
			if (thread->quit == false) {
				thread->quit = true;
				if (--kill == 0) {
					break;
				}
			}
			thread = thread->next;
		}
	}
	
	pthread_mutex_unlock(&thread_pool_mutex);
}

#ifdef ENABLE_TOMAHAWK
/* Return information for Tomahawk
 */
int count_threads_in_pool(void) {
	return thread_pool_size;
}

int count_waiting_workers(void) {
	return waiting_workers;
}

int count_threads_marked_quit(void) {
	t_thread_pool *thread;
	int count = 0;

	pthread_mutex_lock(&thread_pool_mutex);

	thread = thread_pool;
	while (thread != NULL) {
		if (thread->quit) {
			count++;
		}
		thread = thread->next;
	}

	pthread_mutex_unlock(&thread_pool_mutex);

	return count;
}
#endif

#endif
