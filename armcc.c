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

const char* armcc_name[] = {"armcc", NULL};

static const struct compopt armcc_compopts[] = {
	{"--apcs",                      TAKES_ARG},
	{"--arm_linux_conifg_file",	    TAKES_ARG | TAKES_CONCAT_ARG | TAKES_PATH},
	{"--asm",                       TOO_HARD}, /* Need handle two output files to support this.*/
	{"--asm_dir",                   TAKES_ARG | TAKES_PATH},
	{"--brief_diagnostics",         AFFECTS_CPP},
	{"--bss_threshold",             TAKES_ARG},
	{"--compatible",                TAKES_ARG},
	{"--configure_cpp_headers",     AFFECTS_CPP | TAKES_ARG | TAKES_PATH},
	{"--configure_extra_includes",  AFFECTS_CPP | TAKES_ARG | TAKES_CONCAT_ARG | TAKES_PATH},
	{"--configure_extra_libraries", TAKES_ARG | TAKES_CONCAT_ARG | TAKES_PATH},
	{"--configure_gas",             TAKES_ARG | TAKES_PATH},
	{"--configure_gcc",             TAKES_ARG | TAKES_PATH},
	{"--configure_gcc_version",     TAKES_ARG},
	{"--configure_sysroot",         TAKES_ARG | TAKES_PATH},
	{"--cpu",                       TAKES_ARG},
	{"--create_pch",                TOO_HARD}, /*TODO:Support pch later. */
	{"--default_definition_visibility",	TAKES_ARG},
	{"--default_extension",	        TOO_HARD | TAKES_ARG}, /* TODO:Support this later */
	{"--depend_format",             TAKES_ARG},
	{"--diag_error",                TAKES_ARG},
	{"--diag_remark",               TAKES_ARG},
	{"--diag_warning",              TAKES_ARG},
	{"--feedback", 		            TAKES_ARG},/*TODO:Study more about this. This could be TOO_HARD*/
	{"--no_code_gen",               TOO_HARD},
	{"-C",                          AFFECTS_CPP},
	{"-D",                          AFFECTS_CPP | TAKES_ARG | TAKES_CONCAT_ARG},
	{"-E",                          TOO_HARD},
	{"-I",                          AFFECTS_CPP | TAKES_ARG | TAKES_CONCAT_ARG | TAKES_PATH},
	{"-U",                          AFFECTS_CPP | TAKES_ARG | TAKES_CONCAT_ARG},
};

void armcc_init_entry()
{ 
	init_compopts(armcc_compopts, sizeof(armcc_compopts)/sizeof(armcc_compopts[0]));
}

/*
 * Process the compiler options into options suitable for passing to the
 * preprocessor and the real compiler. The preprocessor options don't include
 * -E; this is added later. Returns true on success, otherwise false.
 */
bool
armcc_process_args(struct args *orig_args, struct args **preprocessor_args,
                struct args **compiler_args)
{
	int i;
	bool found_c_opt = false;
	bool found_S_opt = false;
	bool found_pch = false;
	bool found_fpch_preprocess = false;
	const char *actual_language;          /* Language to actually use. */
	const char *input_charset = NULL;
	struct stat st;
	/* is the dependency makefile target name specified ? */
	bool dependency_target_specified = false;
	char *dep_file = NULL, *dep_dir = NULL;
	struct args *stripped_args = NULL, *dep_args = NULL;
	int argc = orig_args->argc;
	char **argv = orig_args->argv;
	bool result = true;
	/* 0: Choose preprocessor type by the file extension.
	 * 1: Use c preprocessor.
	 * 2: Use c++ preprocessor.*/
	unsigned force_preprocessor_type = 0;

	stripped_args = args_init(0, NULL);
	dep_args = args_init(0, NULL);

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
		if (compopt_too_hard(argv[i])
		    || str_startswith(argv[i], "@")
		    || str_startswith(argv[i], "-fdump-")) {
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
		if (str_eq(argv[i], "-S")) {
			args_add(stripped_args, argv[i]);
			found_S_opt = true;
			continue;
		}

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


		/* If multiple source type options are there, the armcc will use the last one. */
		if (str_eq(argv[i], "--cpp")) { 
			force_preprocessor_type = 2;
			continue;
		}
		else if (str_eq(argv[i], "--c90") || str_eq(argv[i], "--c99")) { 
			force_preprocessor_type = 1;
			continue;
		}


		/* The rvct started supporting --depend_target from 4.0.
		 * And there is a bug when using -E and --depend together with rvct which version is earlier than 4.0_697.
		 * That is too hard to support "--depend" for the earlier version of rvct.*/
		if (str_startswith(argv[i], "--depend_dir")) {
			/* We just concat the dir and the filename and pass the result 
			 * to --depend. */
			if (i >= argc - 1) {
				cc_log("Missing argument to %s", argv[i]);
				stats_update(STATS_ARGS);
				result = false;
				goto out;
			}
			dep_dir= argv[i+1];
			i++;
			continue;
		}
		else if (str_startswith(argv[i], "--depend_target")) 
		{
			if (i >= argc - 1) {
				cc_log("Missing argument to %s", argv[i]);
				stats_update(STATS_ARGS);
				result = false;
				goto out;
			}
			dependency_target_specified = true;
			args_add(dep_args, argv[i]);
			args_add(dep_args, argv[i+1]);
			i++;
			continue;
		}
		else if (str_startswith(argv[i], "--depend")) 
		{
			generating_dependencies = true;

			if (i >= argc - 1) {
				cc_log("Missing argument to %s", argv[i]);
				stats_update(STATS_ARGS);
				result = false;
				goto out;
			}
			dep_file = argv[i + 1];
			i++;
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
			char *pchpath;
			if (i == argc-1) {
				cc_log("Missing argument to %s", argv[i]);
				stats_update(STATS_ARGS);
				result = false;
				goto out;
			}

			args_add(stripped_args, argv[i]);
			relpath = make_relative_path(x_strdup(argv[i+1]));
			args_add(stripped_args, relpath);

			/* Try to be smart about detecting precompiled headers */
			pchpath = format("%s.gch", argv[i+1]);
			if (stat(pchpath, &st) == 0) {
				cc_log("Detected use of precompiled header: %s", pchpath);
				found_pch = true;
			}

			free(pchpath);
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

	if (found_pch || found_fpch_preprocess) {
		using_precompiled_header = true;
		if (!(sloppiness & SLOPPY_TIME_MACROS)) {
			cc_log("You have to specify \"time_macros\" sloppiness when using"
			       " precompiled headers to get direct hits");
			cc_log("Disabling direct mode");
			stats_update(STATS_CANTUSEPCH);
			result = false;
			goto out;
		}
	}

	if(force_preprocessor_type == 0) { 
		actual_language = language_for_file(input_file);
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
			p[1] = found_S_opt ? 's' : 'o';
			p[2] = 0;
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
	 *
	 * -finput-charset=XXX (otherwise conversion happens twice)
	 * -x XXX (otherwise the wrong language is selected)
	 */
	*preprocessor_args = args_copy(stripped_args);
	if (input_charset) {
		args_add(*preprocessor_args, input_charset);
	}
	if (found_pch) {
		args_add(*preprocessor_args, "-fpch-preprocess");
	}

	/*
	 * Add flags for dependency generation only to the preprocessor command line.
	 */
	if (generating_dependencies) {
		char *dep_path;
		if (!dependency_target_specified) {
			args_add(dep_args, "--depend_target");
			args_add(dep_args, output_obj);
		}

		free(output_dep);
		args_add(dep_args, "--depend");
		
		if(dep_dir)
		{ 
#ifdef _WIN32
			dep_path = make_relative_path(format("%s\\%s", dep_dir, dep_file));
#else
			dep_path = make_relative_path(format("%s/%s", dep_dir, dep_file));
#endif
		}
		else
		{ 
			dep_path = x_strdup(dep_file);
		}

		args_add(dep_args, dep_path);
		/* dep_path will be free in make_relative_path */
		output_dep = make_relative_path(x_strdup(dep_file));
	}

	if (compile_preprocessed_source_code) {
		*compiler_args = args_copy(stripped_args);
	} else {
		*compiler_args = args_copy(*preprocessor_args);
	}

	i_extension = getenv("CCACHE_EXTENSION");
	if (!i_extension) {
		const char *p_language = p_language_for_language(actual_language);
		i_extension = extension_for_language(p_language) + 1;

	}
	/* Patch for preprocessed file extension for armcc 3.1.
	 * armcc 3.1 cannot recognize "i" or "ii" as the preprocessed source file 
	 * without --compile_all_input. */
	args_add(*compiler_args, "--compile_all_input");
	if (str_eq(i_extension, "ii")) { 
		args_add(*compiler_args, "--cpp");
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
	return result;
}
