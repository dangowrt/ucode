/*
 * Copyright (C) 2020-2021 Jo-Philipp Wich <jo@mein.io>
 *
 * Permission to use, copy, modify, and/or distribute this software for any
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
#include <stdbool.h>
#include <stdint.h>
#include <unistd.h>
#include <errno.h>
#include <ctype.h>
#include <sys/stat.h>

#ifdef JSONC
	#include <json.h>
#else
	#include <json-c/json.h>
#endif

#include "compiler.h"
#include "lexer.h"
#include "lib.h"
#include "vm.h"
#include "source.h"


static void
print_usage(const char *app)
{
	printf(
	"== Usage ==\n\n"
	"  # %s [-d] [-l] [-r] [-S] [-e '[prefix=]{\"var\": ...}'] [-E [prefix=]env.json] {-i <file> | -s \"ucode script...\"}\n"
	"  -h, --help	Print this help\n"
	"  -i file	Specify an ucode script to parse\n"
	"  -s \"ucode script...\"	Specify an ucode fragment to parse\n"
	"  -d Instead of executing the script, dump the resulting AST as dot\n"
	"  -l Do not strip leading block whitespace\n"
	"  -r Do not trim trailing block newlines\n"
	"  -S Enable strict mode\n"
	"  -e Set global variables from given JSON object\n"
	"  -E Set global variables from given JSON file\n"
	"  -m Preload given module\n",
		basename(app));
}

static void
globals_init(uc_prototype *scope)
{
	json_object *arr = xjs_new_array();
	const char *p, *last;

	for (p = last = LIB_SEARCH_PATH;; p++) {
		if (*p == ':' || *p == '\0') {
			json_object_array_add(arr, xjs_new_string_len(last, p - last));

			if (!*p)
				break;

			last = p + 1;
		}
	}

	json_object_object_add(scope->header.jso, "REQUIRE_SEARCH_PATH", arr);
}

static void
register_variable(uc_prototype *scope, const char *key, json_object *val)
{
	char *name = strdup(key);
	char *p;

	if (!name)
		return;

	for (p = name; *p; p++)
		if (!isalnum(*p) && *p != '_')
			*p = '_';

	json_object_object_add(scope->header.jso, name, val);
	free(name);
}


static int
parse(uc_parse_config *config, uc_source *src,
      bool skip_shebang, json_object *env, json_object *modules)
{
	uc_prototype *globals = uc_prototype_new(NULL), *rootscope = NULL;
	uc_function *entry;
	uc_vm vm = {};
	char c, c2, *err;
	int rc = 0;

	uc_vm_init(&vm, config);

	if (skip_shebang) {
		c = fgetc(src->fp);
		c2 = fgetc(src->fp);

		if (c == '#' && c2 == '!') {
			while ((c = fgetc(src->fp)) != EOF) {
				src->off++;

				if (c == '\n')
					break;
			}
		}
		else {
			ungetc(c2, src->fp);
			ungetc(c, src->fp);
		}
	}

	entry = uc_compile(config, src, &err);

	if (!entry) {
		fprintf(stderr, "%s", err);
		free(err);
		rc = 2;
		goto out;
	}

	/* load global variables */
	globals_init(globals);

	/* load env variables */
	if (env) {
		json_object_object_foreach(env, key, val)
			register_variable(globals, key, uc_value_get(val));
	}

	/* load std functions into global scope */
	uc_lib_init(globals);

	/* create instance of global scope, set "global" property on it */
	rootscope = uc_protoref_new(xjs_new_object(), globals);

	json_object_object_add(rootscope->header.jso, "global",
		uc_value_get(globals->header.jso));

	rc = uc_vm_execute(&vm, entry, rootscope, modules);

	if (rc) {
		rc = 1;
		goto out;
	}

out:
	uc_vm_free(&vm);
	uc_value_put(globals->header.jso);

	if (rootscope)
		uc_value_put(rootscope->header.jso);

	return rc;
}

static uc_source *
read_stdin(char **ptr)
{
	size_t rlen = 0, tlen = 0;
	char buf[128];

	if (*ptr) {
		fprintf(stderr, "Can read from stdin only once\n");
		errno = EINVAL;

		return NULL;
	}

	while (true) {
		rlen = fread(buf, 1, sizeof(buf), stdin);

		if (rlen == 0)
			break;

		*ptr = xrealloc(*ptr, tlen + rlen);
		memcpy(*ptr + tlen, buf, rlen);
		tlen += rlen;
	}

	return uc_source_new_buffer("[stdin]", *ptr, tlen);
}

static json_object *
parse_envfile(FILE *fp)
{
	json_object *rv = NULL;
	enum json_tokener_error err;
	struct json_tokener *tok;
	char buf[128];
	size_t rlen;

	tok = xjs_new_tokener();

	while (true) {
		rlen = fread(buf, 1, sizeof(buf), fp);

		if (rlen == 0)
			break;

		rv = json_tokener_parse_ex(tok, buf, rlen);
		err = json_tokener_get_error(tok);

		if (err != json_tokener_continue)
			break;
	}

	if (err != json_tokener_success || !json_object_is_type(rv, json_type_object)) {
		json_object_put(rv);
		rv = NULL;
	}

	json_tokener_free(tok);

	return rv;
}

int
main(int argc, char **argv)
{
	json_object *env = NULL, *modules = NULL, *o, *p;
	uc_source *source = NULL, *envfile = NULL;
	char *stdin = NULL, *c;
	bool shebang = false;
	int opt, rv = 0;

	uc_parse_config config = {
		.strict_declarations = false,
		.lstrip_blocks = true,
		.trim_blocks = true
	};

	if (argc == 1)
	{
		print_usage(argv[0]);
		goto out;
	}

	while ((opt = getopt(argc, argv, "hlrSe:E:i:s:m:")) != -1)
	{
		switch (opt) {
		case 'h':
			print_usage(argv[0]);
			goto out;

		case 'i':
			if (source)
				fprintf(stderr, "Options -i and -s are exclusive\n");

			if (!strcmp(optarg, "-"))
				source = read_stdin(&stdin);
			else
				source = uc_source_new_file(optarg);

			if (!source) {
				fprintf(stderr, "Failed to open %s: %s\n", optarg, strerror(errno));
				rv = 1;
				goto out;
			}

			break;

		case 'l':
			config.lstrip_blocks = false;
			break;

		case 'r':
			config.trim_blocks = false;
			break;

		case 's':
			if (source)
				fprintf(stderr, "Options -i and -s are exclusive\n");

			source = uc_source_new_buffer("[-s argument]", xstrdup(optarg), strlen(optarg));
			break;

		case 'S':
			config.strict_declarations = true;
			break;

		case 'e':
			c = strchr(optarg, '=');

			if (c)
				*c++ = 0;
			else
				c = optarg;

			envfile = uc_source_new_buffer("[-e argument]", xstrdup(c), strlen(c));
			/* fallthrough */

		case 'E':
			if (!envfile) {
				c = strchr(optarg, '=');

				if (c)
					*c++ = 0;
				else
					c = optarg;

				if (!strcmp(c, "-"))
					envfile = read_stdin(&stdin);
				else
					envfile = uc_source_new_file(c);

				if (!envfile) {
					fprintf(stderr, "Failed to open %s: %s\n", c, strerror(errno));
					rv = 1;
					goto out;
				}
			}

			o = parse_envfile(envfile->fp);

			uc_source_put(envfile);

			envfile = NULL;

			if (!o) {
				fprintf(stderr, "Option -%c must point to a valid JSON object\n", opt);
				rv = 1;
				goto out;
			}

			env = env ? env : xjs_new_object();

			if (c > optarg && optarg[0]) {
				p = xjs_new_object();
				json_object_object_add(env, optarg, p);
			}
			else {
				p = env;
			}

			json_object_object_foreach(o, key, val)
				json_object_object_add(p, key, json_object_get(val));

			json_object_put(o);

			break;

		case 'm':
			modules = modules ? modules : xjs_new_array();

			json_object_array_add(modules, xjs_new_string(optarg));

			break;
		}
	}

	if (!source && argv[optind] != NULL) {
		source = uc_source_new_file(argv[optind]);

		if (!source) {
			fprintf(stderr, "Failed to open %s: %s\n", argv[optind], strerror(errno));
			rv = 1;
			goto out;
		}

		shebang = true;
	}

	if (!source) {
		fprintf(stderr, "One of -i or -s is required\n");
		rv = 1;
		goto out;
	}

	rv = parse(&config, source, shebang, env, modules);

out:
	json_object_put(modules);
	json_object_put(env);

	uc_source_put(source);

	return rv;
}
