/*
 * xpcsvc: claim a launchd MachService with libxpc and log what clients send.
 *
 *   xpcsvc NAME [SECS] [reply]
 *
 * SECS defaults to 60; 0 runs until killed. With "reply", each dictionary
 * message gets an empty reply, which makes a synchronous client handshake
 * fail instead of waiting.
 *
 * The iOS SDK marks xpc_connection_create_mach_service() unavailable, so every
 * libxpc entry point is resolved with dlsym() at runtime. The binary links only
 * libSystem and loads /usr/lib/system/libxpc.dylib explicitly (it is in the
 * dyld shared cache on iOS 16).
 */
#include <dispatch/dispatch.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <xpc/xpc.h>

typedef xpc_connection_t (*create_mach_service_fn)(const char *, dispatch_queue_t, uint64_t);
typedef void (*set_event_handler_fn)(xpc_connection_t, void (^)(xpc_object_t));
typedef void (*resume_fn)(xpc_connection_t);
typedef void (*send_message_fn)(xpc_connection_t, xpc_object_t);
typedef xpc_object_t (*dictionary_create_reply_fn)(xpc_object_t);
typedef xpc_type_t (*get_type_fn)(xpc_object_t);
typedef char *(*copy_description_fn)(xpc_object_t);
typedef void (*release_fn)(xpc_object_t);

static create_mach_service_fn create_mach_service;
static set_event_handler_fn set_event_handler;
static resume_fn conn_resume;
static send_message_fn send_message;
static dictionary_create_reply_fn dictionary_create_reply;
static get_type_fn get_type;
static copy_description_fn copy_description;
static release_fn xpc_release_fn;

static int send_empty_reply;

static void *must_sym(void *lib, const char *name)
{
	void *p = dlsym(lib, name);
	if (!p) {
		fprintf(stderr, "dlsym %s: %s\n", name, dlerror());
		exit(1);
	}
	return p;
}

static void load_libxpc(void)
{
	void *lib = NULL;
	const char *paths[] = {
		"/usr/lib/system/libxpc.dylib",
		"/usr/lib/libxpc.dylib",
		"libxpc.dylib",
	};
	for (size_t i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
		lib = dlopen(paths[i], RTLD_NOW | RTLD_LOCAL);
		if (lib)
			break;
	}
	if (!lib) {
		fprintf(stderr, "dlopen libxpc: %s\n", dlerror());
		exit(1);
	}
	create_mach_service = (create_mach_service_fn)must_sym(lib, "xpc_connection_create_mach_service");
	set_event_handler = (set_event_handler_fn)must_sym(lib, "xpc_connection_set_event_handler");
	conn_resume = (resume_fn)must_sym(lib, "xpc_connection_resume");
	send_message = (send_message_fn)must_sym(lib, "xpc_connection_send_message");
	dictionary_create_reply = (dictionary_create_reply_fn)must_sym(lib, "xpc_dictionary_create_reply");
	get_type = (get_type_fn)must_sym(lib, "xpc_get_type");
	copy_description = (copy_description_fn)must_sym(lib, "xpc_copy_description");
	xpc_release_fn = (release_fn)must_sym(lib, "xpc_release");
}

static void log_event(const char *what, xpc_object_t event)
{
	char *text = copy_description(event);
	printf("%s: %s\n", what, text ? text : "?");
	fflush(stdout);
	free(text);
}

static void peer_event(xpc_connection_t peer, xpc_object_t event)
{
	/*
	 * Connection objects are the only events whose type matches the listener
	 * itself; dictionaries and errors do not. This avoids needing the
	 * _xpc_type_* data symbols.
	 */
	log_event("message", event);
	if (send_empty_reply && get_type(event) != get_type((xpc_object_t)peer)) {
		xpc_object_t reply = dictionary_create_reply(event);
		if (reply) {
			printf("replying empty\n");
			fflush(stdout);
			send_message(peer, reply);
			xpc_release_fn(reply);
		}
	}
}

int main(int argc, char **argv)
{
	if (argc < 2) {
		fprintf(stderr, "usage: xpcsvc NAME [SECS] [reply]\n");
		return 2;
	}
	send_empty_reply = argc > 3 && !strcmp(argv[3], "reply");
	int secs = argc > 2 ? atoi(argv[2]) : 60;

	load_libxpc();

	xpc_connection_t listener = create_mach_service(
		argv[1], NULL, XPC_CONNECTION_MACH_SERVICE_LISTENER);
	if (!listener) {
		fprintf(stderr, "xpc_connection_create_mach_service failed\n");
		return 1;
	}
	set_event_handler(listener, ^(xpc_object_t event) {
		if (get_type(event) == get_type((xpc_object_t)listener)) {
			printf("connection\n");
			fflush(stdout);
			set_event_handler(event, ^(xpc_object_t message) {
				peer_event(event, message);
			});
			conn_resume(event);
		} else {
			log_event("listener event", event);
		}
	});
	conn_resume(listener);
	printf("listening on %s for %d s (reply=%d)\n", argv[1], secs, send_empty_reply);
	fflush(stdout);

	if (secs > 0)
		alarm((unsigned)secs);
	dispatch_main();
	return 0;
}
