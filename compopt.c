/*
 * Copyright (C) 2010 Joel Rosdahl
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 3 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program; if not, write to the Free Software Foundation, Inc., 51
 * Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include "ccache.h"
#include "compopt.h"

static const struct compopt *compopts;
static unsigned int compopts_count;

void init_compopts(const struct compopt *compopts_in, unsigned int compots_c)
{ 
	compopts = compopts_in;
	compopts_count = compots_c;
}

static int
compare_compopts(const void *key1, const void *key2)
{
	const struct compopt *opt1 = (const struct compopt *)key1;
	const struct compopt *opt2 = (const struct compopt *)key2;
	return strcmp(opt1->name, opt2->name);
}

static const struct compopt *
find(const char *option)
{
	struct compopt key;
	key.name = option;
	return bsearch(
		&key, compopts, compopts_count,
		sizeof(compopts[0]), compare_compopts);
}

/* Runs fn on the first two characters of option. */
bool
compopt_short(bool (*fn)(const char *), const char *option)
{
	char *short_opt = x_strndup(option, 2);
	bool retval = fn(short_opt);
	free(short_opt);
	return retval;
}

/* For test purposes. */
bool
compopt_verify_sortedness(void)
{
	size_t i;
	for (i = 1; i < compopts_count; i++) {
		if (strcmp(compopts[i-1].name, compopts[i].name) >= 0) {
			fprintf(stderr,
			        "compopt_verify_sortedness: %s >= %s\n",
			        compopts[i-1].name,
			        compopts[i].name);
			return false;
		}
	}
	return true;
}

bool
compopt_affects_cpp(const char *option)
{
	const struct compopt *co = find(option);
	return co && (co->type & AFFECTS_CPP);
}

bool
compopt_too_hard(const char *option)
{
	const struct compopt *co = find(option);
	return co && (co->type & TOO_HARD);
}

bool
compopt_too_hard_for_direct_mode(const char *option)
{
	const struct compopt *co = find(option);
	return co && (co->type & TOO_HARD_DIRECT);
}

bool
compopt_takes_path(const char *option)
{
	const struct compopt *co = find(option);
	return co && (co->type & TAKES_PATH);
}

bool
compopt_takes_arg(const char *option)
{
	const struct compopt *co = find(option);
	return co && (co->type & TAKES_ARG);
}
