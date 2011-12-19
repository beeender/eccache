#include "ccache.h"
#include "compopt.h"
#include "language.h"

extern bool enable_direct;
extern char *input_file;
extern char *output_obj;
extern bool enable_unify;
extern bool compile_preprocessed_source_code;
extern bool generating_dependencies;
extern char *output_dep;
extern bool using_precompiled_header;
extern unsigned sloppiness;
extern bool output_is_precompiled_header;
extern bool direct_i_file;
extern const char *i_extension;

const char* c166_name[] = {"cc166", NULL};

static const struct compopt c166_compopts[] = {
	{"-D",				AFFECTS_CPP | TAKES_ARG | TAKES_CONCAT_ARG},
	{"-E",				TOO_HARD},
	{"-I",				AFFECTS_CPP | TAKES_ARG | TAKES_CONCAT_ARG | TAKES_PATH},
	{"-T",				AFFECTS_CPP | TAKES_ARG},
	{"-U",				AFFECTS_CPP | TAKES_ARG | TAKES_CONCAT_ARG},
	{"-Wa",				TAKES_ARG},
	{"-Wc",				TAKES_ARG},
	{"-Wcp",			TAKES_ARG},
	{"-Wf",				TAKES_ARG},
	{"-Wl",				TAKES_ARG},
	{"-Wm",				AFFECTS_CPP | TAKES_ARG},
	{"-Wo",				TAKES_ARG},
	{"-Wpl",			TAKES_ARG},
	{"-cc",				TOO_HARD},
	{"-cf",				TOO_HARD},
	{"-cl",				TOO_HARD},
	{"-cp",				TOO_HARD},
	{"-cprep",			TOO_HARD}, /*Not support for now. Maybe future.*/
	{"-cs",				TOO_HARD},
	{"-e",				TOO_HARD}, 
	{"-err",			TOO_HARD}, 
	{"-exit",			TOO_HARD}, 
	{"-f",				TOO_HARD}, /*Not support for now. Maybe future.*/
	{"-gso",			TOO_HARD},
	{"-ieee",			TOO_HARD},
	{"-ihex",			TOO_HARD},
	{"-lib",			TAKES_ARG},
	{"-m",              AFFECTS_CPP | TAKES_ARG},
	{"-n",				TOO_HARD},
 	/* Actually the -s doesn't do anything when the cc166 invoked.
	 * It will only take effect while invoking c166. */
	/* {"-s",              AFFECTS_CPP }, */
	{"-srec",			TOO_HARD},
	{"-tmp",			TOO_HARD},
	{"-v",				TOO_HARD},
	{"-v0",				TOO_HARD},
	{"-x",              AFFECTS_CPP | TAKES_ARG},
	{"-z",              AFFECTS_CPP | TAKES_ARG},
};

void c166_init_entry()
{ 
	init_compopts(c166_compopts, sizeof(c166_compopts)/sizeof(c166_compopts[0]));
}

/*
 * Process the compiler options into options suitable for passing to the
 * preprocessor and the real compiler. The preprocessor options don't include
 * -E; this is added later. Returns true on success, otherwise false.
 */
bool
c166_process_args(struct args *orig_args, struct args **preprocessor_args,
                struct args **compiler_args)
{
	int i;
	bool found_c_opt = false;
	bool found_S_opt = false;
	bool found_H_opt = false;
	/* 0: Choose preprocessor type by the file extension.
	 * 1: Use c preprocessor.
	 * 2: Use c++ preprocessor.*/
	unsigned force_preprocessor_type = 0;
	const char *file_language;            /* As deduced from file extension. */
	const char *actual_language;          /* Language to actually use. */
	struct stat st;
	/* is the dependency makefile name overridden with -MF? */
	bool dependency_filename_specified = false;
	/* is the dependency makefile target name specified with -MT or -MQ? */
	bool dependency_target_specified = false;
	struct args *stripped_args = NULL, *dep_args = NULL, *h_args;
	int argc = orig_args->argc;
	char **argv = orig_args->argv;
	bool result = true;

	stripped_args = args_init(0, NULL);
	dep_args = args_init(0, NULL);
	h_args = args_init(0, NULL);

	args_add(stripped_args, argv[0]);

	for (i = 1; i < argc; i++) {
		/* The user knows best: just swallow the next arg */
		if (str_eq(argv[i], "--ccache-skip")) {
			i++;
			if (i == argc) {
				cc_log("--ccache-skip lacks an argument");
				result = false;
				goto out;
			}
			args_add(stripped_args, argv[i]);
			continue;
		}

		/* Special case for -E. */
		if (str_eq(argv[i], "-E")) {
			stats_update(STATS_PREPROCESSING);
			result = false;
			goto out;
		}

		/* These are always too hard. */
		if (compopt_too_hard(argv[i])) {
			cc_log("Compiler option %s is unsupported", argv[i]);
			stats_update(STATS_UNSUPPORTED);
			result = false;
			goto out;
		}

		/* These are too hard in direct mode. */
		if (enable_direct) {
			if (compopt_too_hard_for_direct_mode(argv[i])) {
				cc_log("Unsupported compiler option for direct mode: %s", argv[i]);
				enable_direct = false;
			}
		}

		/* we must have -c */
		if (str_eq(argv[i], "-c")) {
			args_add(stripped_args, argv[i]);
			found_c_opt = true;
			continue;
		}

		/* -S changes the default extension */
		/* TODO: Check this -S out!
		if (str_eq(argv[i], "-S")) {
			args_add(stripped_args, argv[i]);
			found_S_opt = true;
			continue;
		}
		*/

		/* we need to work out where the output was meant to go */
		if (str_eq(argv[i], "-o")) {
			if (i == argc-1) {
				cc_log("Missing argument to %s", argv[i]);
				stats_update(STATS_ARGS);
				result = false;
				goto out;
			}
			output_obj = argv[i+1];
			i++;
			continue;
		}

		/* alternate form of -o, with no space */
		if (str_startswith(argv[i], "-o")) {
			output_obj = &argv[i][2];
			continue;
		}

		/* debugging is handled specially, so that we know if we
		   can strip line number info
		*/
		if (str_startswith(argv[i], "-g")) {
			args_add(stripped_args, argv[i]);
			if (enable_unify) {
				cc_log("%s used; disabling unify mode", argv[i]);
				enable_unify = false;
			}
			continue;
		}

		if (str_startswith(argv[i], "-H")) {
			cc_log("Detected -H %s", argv[i]);
			args_add(h_args, argv[i]);
			found_H_opt = true;
			continue;
		}

		/*
		 * Options taking an argument that that we may want to rewrite
		 * to relative paths to get better hit rate. A secondary effect
		 * is that paths in the standard error output produced by the
		 * compiler will be normalized.
		 */
		if (compopt_takes_path(argv[i])) {
			char *relpath;
			if (i == argc-1) {
				cc_log("Missing argument to %s", argv[i]);
				stats_update(STATS_ARGS);
				result = false;
				goto out;
			}

			args_add(stripped_args, argv[i]);
			relpath = make_relative_path(x_strdup(argv[i+1]));
			args_add(stripped_args, relpath);

			free(relpath);
			i++;
			continue;
		}

		/* Same as above but options with concatenated argument. */
		if (compopt_short(compopt_takes_path, argv[i])) {
			char *relpath;
			char *option;
			relpath = make_relative_path(x_strdup(argv[i] + 2));
			option = format("-%c%s", argv[i][1], relpath);
			args_add(stripped_args, option);
			free(relpath);
			free(option);
			continue;
		}

		/* options that take an argument */
		if (compopt_takes_arg(argv[i])) {
			if (i == argc-1) {
				cc_log("Missing argument to %s", argv[i]);
				stats_update(STATS_ARGS);
				result = false;
				goto out;
			}
			args_add(stripped_args, argv[i]);
			args_add(stripped_args, argv[i+1]);
			i++;
			continue;
		}

		if (str_eq(argv[i], "-c++")) {
			force_preprocessor_type = 2;
			args_add(stripped_args, argv[i]);
			continue;
		}
		if (str_eq(argv[i], "-noc++")) {
			force_preprocessor_type = 1;
			args_add(stripped_args, argv[i]);
			continue;
		}

		/* other options */
		if (argv[i][0] == '-') {
			args_add(stripped_args, argv[i]);
			continue;
		}

		/* if an argument isn't a plain file then assume its
		   an option, not an input file. This allows us to
		   cope better with unusual compiler options */
		if (stat(argv[i], &st) != 0 || !S_ISREG(st.st_mode)) {
			cc_log("%s is not a regular file, not considering as input file",
			       argv[i]);
			args_add(stripped_args, argv[i]);
			continue;
		}

		if (input_file) {
			if (language_for_file(argv[i])) {
				cc_log("Multiple input files: %s and %s", input_file, argv[i]);
				stats_update(STATS_MULTIPLE);
			} else if (!found_c_opt) {
				cc_log("Called for link with %s", argv[i]);
				if (strstr(argv[i], "conftest.")) {
					stats_update(STATS_CONFTEST);
				} else {
					stats_update(STATS_LINK);
				}
			} else {
				cc_log("Unsupported source extension: %s", argv[i]);
				stats_update(STATS_SOURCELANG);
			}
			result = false;
			goto out;
		}

		/* Rewrite to relative to increase hit rate. */
		input_file = make_relative_path(x_strdup(argv[i]));
	}

	if (!input_file) {
		cc_log("No input file found");
		stats_update(STATS_NOINPUT);
		result = false;
		goto out;
	}

	file_language = language_for_file(input_file);
	if(force_preprocessor_type == 0) { 
		actual_language = file_language;
	} 
	else if(force_preprocessor_type == 2) { 
		actual_language = "c++";
	}
	else {
		actual_language = "c";
	}
	
	output_is_precompiled_header =
		actual_language && strstr(actual_language, "-header") != NULL;

	if (!found_c_opt && !output_is_precompiled_header) {
		cc_log("No -c option found");
		/* I find that having a separate statistic for autoconf tests is useful,
		   as they are the dominant form of "called for link" in many cases */
		if (strstr(input_file, "conftest.")) {
			stats_update(STATS_CONFTEST);
		} else {
			stats_update(STATS_LINK);
		}
		result = false;
		goto out;
	}

	if (!actual_language) {
		cc_log("Unsupported source extension: %s", input_file);
		stats_update(STATS_SOURCELANG);
		result = false;
		goto out;
	}

	direct_i_file = language_is_preprocessed(actual_language);

	if (output_is_precompiled_header) {
		/* It doesn't work to create the .gch from preprocessed source. */
		cc_log("Creating precompiled header; not compiling preprocessed code");
		compile_preprocessed_source_code = false;
	}

	i_extension = getenv("CCACHE_EXTENSION");
	if (!i_extension) {
		const char *p_language = p_language_for_language(actual_language);
		if(str_eq(p_language, "c++-cpp-output")) { 
			/* Dirty fix for preprocessed file extension for cc166.
			 * The cp166 cannot handle cpp files with extension ii.*/
			i_extension = "ii.cpp";
		}
		else { 
			i_extension = extension_for_language(p_language) + 1;
		}
	}

	/* don't try to second guess the compilers heuristics for stdout handling */
	if (output_obj && str_eq(output_obj, "-")) {
		stats_update(STATS_OUTSTDOUT);
		cc_log("Output file is -");
		result = false;
		goto out;
	}

	if (!output_obj) {
		if (output_is_precompiled_header) {
			output_obj = format("%s.gch", input_file);
		} else {
			char *p;
			output_obj = x_strdup(input_file);
			if ((p = strrchr(output_obj, '/'))) {
				output_obj = p+1;
			}
			p = strrchr(output_obj, '.');
			if (!p || !p[1]) {
				cc_log("Badly formed object filename");
				stats_update(STATS_ARGS);
				result = false;
				goto out;
			}
			*p = 0;
			p = output_obj;
			if(found_S_opt)
			{ 
				output_obj = format("%s.s", p);
			}
			else
			{ /*The default extension of object file is obj for c166.*/
				output_obj = format("%s.obj", p);
			}
			free(p);
		}
	}

	/* cope with -o /dev/null */
	if (!str_eq(output_obj,"/dev/null")
	    && stat(output_obj, &st) == 0
	    && !S_ISREG(st.st_mode)) {
		cc_log("Not a regular file: %s", output_obj);
		stats_update(STATS_DEVICE);
		result = false;
		goto out;
	}

	/*
	 * Some options shouldn't be passed to the real compiler when it compiles
	 * preprocessed code:
	 */
	*preprocessor_args = args_copy(stripped_args);
	/* Args with -H has been already preprocessed.
	 * If it passed to the compiler again, some type redefined error will pop up.*/
	if (found_H_opt) {
		args_extend(*preprocessor_args, h_args);
	}

	/*
	 * Add flags for dependency generation only to the preprocessor command line.
	 */
	if (generating_dependencies) {
		if (!dependency_filename_specified) {
			char *default_depfile_name;
			char *base_name;

			base_name = remove_extension(output_obj);
			default_depfile_name = format("%s.d", base_name);
			free(base_name);
			args_add(dep_args, "-MF");
			args_add(dep_args, default_depfile_name);
			output_dep = make_relative_path(x_strdup(default_depfile_name));
		}

		if (!dependency_target_specified) {
			args_add(dep_args, "-MQ");
			args_add(dep_args, output_obj);
		}
	}

	if (compile_preprocessed_source_code) {
		*compiler_args = args_copy(stripped_args);
	} else {
		*compiler_args = args_copy(*preprocessor_args);
	}
	
	/* Due to bugs or cc166 v8.6r3, the behaviours of c/c++ preprocessor 
	 * are quite different.
	 * When using cpp preprocessor, the output will be directly send to stdout like gcc.
	 * When using c preprocessor, the output will be written to filename.i even without "-o".*/
	if(str_eq(actual_language, "c"))
	{ 
#ifdef _WIN32
#error Never test this in Windows.
#else
		args_add(*preprocessor_args, "-o/dev/stdout");
#endif
	}

	/*
	 * Only pass dependency arguments to the preprocesor since Intel's C++
	 * compiler doesn't produce a correct .d file when compiling preprocessed
	 * source.
	 */
	args_extend(*preprocessor_args, dep_args);

out:
	args_free(stripped_args);
	args_free(dep_args);
	args_free(h_args);

	return result;
}
