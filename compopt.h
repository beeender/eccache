#ifndef CCACHE_COMPOPT_H
#define CCACHE_COMPOPT_H

#include "system.h"

#define TOO_HARD         (1 << 0)
#define TOO_HARD_DIRECT  (1 << 1)
#define TAKES_ARG        (1 << 2)
#define TAKES_CONCAT_ARG (1 << 3)
#define TAKES_PATH       (1 << 4)
#define AFFECTS_CPP      (1 << 5)

struct compopt {
	const char *name;
	int type;
};

void init_compopts(const struct compopt *compopts_in, unsigned int compots_c);
bool compopt_short(bool (*fn)(const char *option), const char *option);
bool compopt_affects_cpp(const char *option);
bool compopt_too_hard(const char *option);
bool compopt_too_hard_for_direct_mode(const char *option);
bool compopt_takes_path(const char *option);
bool compopt_takes_arg(const char *option);

#endif /* CCACHE_COMPOPT_H */
