/*
 * Copyright (c) 2020, The SerenityOS developers.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice, this
 *    list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "Shell.h"
#include "Execution.h"
#include <AK/Function.h>
#include <AK/LexicalPath.h>
#include <AK/ScopeGuard.h>
#include <AK/StringBuilder.h>
#include <LibCore/ArgsParser.h>
#include <LibCore/DirIterator.h>
#include <LibCore/ElapsedTimer.h>
#include <LibCore/Event.h>
#include <LibCore/EventLoop.h>
#include <LibCore/File.h>
#include <LibLine/Editor.h>
#include <errno.h>
#include <fcntl.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

// FIXME: We do not expand variables inside strings
//        if we want to be more sh-like, we should do that some day
static constexpr bool HighlightVariablesInsideStrings = false;
static bool s_disable_hyperlinks = false;
extern RefPtr<Line::Editor> editor;

//#define SH_DEBUG

void Shell::print_path(const String& path)
{
    if (s_disable_hyperlinks) {
        printf("%s", path.characters());
        return;
    }
    printf("\033]8;;file://%s%s\033\\%s\033]8;;\033\\", hostname, path.characters(), path.characters());
}

String Shell::prompt() const
{
    auto build_prompt = [&]() -> String {
        auto* ps1 = getenv("PROMPT");
        if (!ps1) {
            if (uid == 0)
                return "# ";

            StringBuilder builder;
            builder.appendf("\033]0;%s@%s:%s\007", username.characters(), hostname, cwd.characters());
            builder.appendf("\033[31;1m%s\033[0m@\033[37;1m%s\033[0m:\033[32;1m%s\033[0m$> ", username.characters(), hostname, cwd.characters());
            return builder.to_string();
        }

        StringBuilder builder;
        for (char* ptr = ps1; *ptr; ++ptr) {
            if (*ptr == '\\') {
                ++ptr;
                if (!*ptr)
                    break;
                switch (*ptr) {
                case 'X':
                    builder.append("\033]0;");
                    break;
                case 'a':
                    builder.append(0x07);
                    break;
                case 'e':
                    builder.append(0x1b);
                    break;
                case 'u':
                    builder.append(username);
                    break;
                case 'h':
                    builder.append(hostname);
                    break;
                case 'w': {
                    String home_path = getenv("HOME");
                    if (cwd.starts_with(home_path)) {
                        builder.append('~');
                        builder.append(cwd.substring_view(home_path.length(), cwd.length() - home_path.length()));
                    } else {
                        builder.append(cwd);
                    }
                    break;
                }
                case 'p':
                    builder.append(uid == 0 ? '#' : '$');
                    break;
                }
                continue;
            }
            builder.append(*ptr);
        }
        return builder.to_string();
    };

    auto the_prompt = build_prompt();
    auto prompt_metrics = editor->actual_rendered_string_metrics(the_prompt);
    auto prompt_length = prompt_metrics.line_lengths.last();

    if (m_should_continue != ExitCodeOrContinuationRequest::Nothing) {
        const auto format_string = "\033[34m%.*-s\033[m";
        switch (m_should_continue) {
        case ExitCodeOrContinuationRequest::Pipe:
            return String::format(format_string, prompt_length, "pipe> ");
        case ExitCodeOrContinuationRequest::DoubleQuotedString:
            return String::format(format_string, prompt_length, "dquote> ");
        case ExitCodeOrContinuationRequest::SingleQuotedString:
            return String::format(format_string, prompt_length, "squote> ");
        default:
            break;
        }
    }
    return the_prompt;
}

int Shell::builtin_bg(int argc, const char** argv)
{
    int job_id = -1;

    Core::ArgsParser parser;
    parser.add_positional_argument(job_id, "Job id to run in background", "job_id", Core::ArgsParser::Required::No);

    if (!parser.parse(argc, const_cast<char**>(argv), false))
        return 1;

    if (job_id == -1 && !jobs.is_empty())
        job_id = find_last_job_id();

    Job* job = nullptr;

    for (auto& entry : jobs) {
        if (entry.value->job_id() == (u64)job_id) {
            job = entry.value;
            break;
        }
    }
    if (!job) {
        if (job_id == -1) {
            printf("bg: no current job\n");
        } else {
            printf("bg: job with id %d not found\n", job_id);
        }
        return 1;
    }

    job->set_running_in_background(true);

    dbg() << "Resuming " << job->pid() << " (" << job->cmd() << ")";
    printf("Resuming job %llu - %s\n", job->job_id(), job->cmd().characters());

    if (killpg(job->pgid(), SIGCONT) < 0) {
        perror("killpg");
        return 1;
    }

    return 0;
}

int Shell::builtin_cd(int argc, const char** argv)
{
    const char* arg_path = nullptr;

    Core::ArgsParser parser;
    parser.add_positional_argument(arg_path, "Path to change to", "path", Core::ArgsParser::Required::No);

    if (!parser.parse(argc, const_cast<char**>(argv), false))
        return 1;

    String new_path;

    if (!arg_path) {
        new_path = home;
        if (cd_history.is_empty() || cd_history.last() != home)
            cd_history.enqueue(home);
    } else {
        if (cd_history.is_empty() || cd_history.last() != arg_path)
            cd_history.enqueue(arg_path);
        if (strcmp(argv[1], "-") == 0) {
            char* oldpwd = getenv("OLDPWD");
            if (oldpwd == nullptr)
                return 1;
            new_path = oldpwd;
        } else if (arg_path[0] == '/') {
            new_path = argv[1];
        } else {
            StringBuilder builder;
            builder.append(cwd);
            builder.append('/');
            builder.append(arg_path);
            new_path = builder.to_string();
        }
    }

    LexicalPath lexical_path(new_path);
    if (!lexical_path.is_valid()) {
        printf("LexicalPath failed to canonicalize '%s'\n", new_path.characters());
        return 1;
    }
    const char* path = lexical_path.string().characters();

    struct stat st;
    int rc = stat(path, &st);
    if (rc < 0) {
        printf("stat(%s) failed: %s\n", path, strerror(errno));
        return 1;
    }
    if (!S_ISDIR(st.st_mode)) {
        printf("Not a directory: %s\n", path);
        return 1;
    }
    rc = chdir(path);
    if (rc < 0) {
        printf("chdir(%s) failed: %s\n", path, strerror(errno));
        return 1;
    }
    setenv("OLDPWD", cwd.characters(), 1);
    cwd = lexical_path.string();
    setenv("PWD", cwd.characters(), 1);
    return 0;
}

int Shell::builtin_cdh(int argc, const char** argv)
{
    int index = -1;

    Core::ArgsParser parser;
    parser.add_positional_argument(index, "Index of the cd history entry (leave out for a list)", "index", Core::ArgsParser::Required::No);

    if (!parser.parse(argc, const_cast<char**>(argv), false))
        return 1;

    if (index == -1) {
        if (cd_history.size() == 0) {
            printf("cdh: no history available\n");
            return 0;
        }

        for (int i = cd_history.size() - 1; i >= 0; --i)
            printf("%lu: %s\n", cd_history.size() - i, cd_history.at(i).characters());
        return 0;
    }

    if (index < 1 || (size_t)index > cd_history.size()) {
        fprintf(stderr, "cdh: history index out of bounds: %d not in (0, %zu)\n", index, cd_history.size());
        return 1;
    }

    const char* path = cd_history.at(cd_history.size() - index).characters();
    const char* cd_args[] = { "cd", path };
    return Shell::builtin_cd(2, cd_args);
}

int Shell::builtin_dirs(int argc, const char** argv)
{
    // The first directory in the stack is ALWAYS the current directory
    directory_stack.at(0) = cwd.characters();

    if (argc == 1) {
        for (auto& directory : directory_stack) {
            print_path(directory);
            fputc(' ', stdout);
        }

        printf("\n");
        return 0;
    }

    bool clear = false;
    bool print = false;
    bool number_when_printing = false;

    Vector<const char*> paths;

    Core::ArgsParser parser;
    parser.add_option(clear, "Clear the directory stack", "clear", 'c');
    parser.add_option(print, "Print directory entries", "print", 'p');
    parser.add_option(number_when_printing, "Number the directories in the stack when printing", "number", 'v');
    parser.add_positional_argument(paths, "Extra paths to put on the stack", "paths", Core::ArgsParser::Required::No);

    if (!parser.parse(argc, const_cast<char**>(argv), false))
        return 1;

    // -v implies -p
    print = print || number_when_printing;

    if (clear) {
        for (size_t i = 1; i < directory_stack.size(); i++)
            directory_stack.remove(i);
    }

    for (auto& path : paths)
        directory_stack.append(path);

    if (print) {
        auto idx = 0;
        for (auto& directory : directory_stack) {
            if (number_when_printing)
                printf("%d ", idx++);
            print_path(directory);
            fputc('\n', stdout);
        }
    }

    return 0;
}

int Shell::builtin_exit(int, const char**)
{
    if (!jobs.is_empty()) {
        if (!m_should_ignore_jobs_on_next_exit) {
            printf("Shell: You have %zu active job%s, run 'exit' again to really exit.\n", jobs.size(), jobs.size() > 1 ? "s" : "");
            m_should_ignore_jobs_on_next_exit = true;
            return 1;
        }
    }
    stop_all_jobs();
    save_history();
    printf("Good-bye!\n");
    exit(0);
    return 0;
}

int Shell::builtin_export(int argc, const char** argv)
{
    Vector<const char*> vars;

    Core::ArgsParser parser;
    parser.add_positional_argument(vars, "List of variable[=value]'s", "values", Core::ArgsParser::Required::No);

    if (!parser.parse(argc, const_cast<char**>(argv), false))
        return 1;

    if (vars.size() == 0) {
        for (int i = 0; environ[i]; ++i)
            puts(environ[i]);
        return 0;
    }

    int return_value = 0;

    for (auto& value : vars) {
        auto parts = String { value }.split_limit('=', 2);

        if (parts.size() == 1) {
            parts.append("");
        }

        int setenv_return = setenv(parts[0].characters(), parts[1].characters(), 1);

        if (setenv_return != 0) {
            perror("setenv");
            return_value = 1;
            break;
        }

        if (parts[0] == "PATH")
            cache_path();
    }

    return return_value;
}

int Shell::builtin_fg(int argc, const char** argv)
{
    int job_id = -1;

    Core::ArgsParser parser;
    parser.add_positional_argument(job_id, "Job id to bring to foreground", "job_id", Core::ArgsParser::Required::No);

    if (!parser.parse(argc, const_cast<char**>(argv), false))
        return 1;

    if (job_id == -1 && !jobs.is_empty())
        job_id = find_last_job_id();

    Job* job = nullptr;

    for (auto& entry : jobs) {
        if (entry.value->job_id() == (u64)job_id) {
            job = entry.value;
            break;
        }
    }
    if (!job) {
        if (job_id == -1) {
            printf("fg: no current job\n");
        } else {
            printf("fg: job with id %d not found\n", job_id);
        }
        return 1;
    }

    job->set_running_in_background(false);

    dbg() << "Resuming " << job->pid() << " (" << job->cmd() << ")";
    printf("Resuming job %llu - %s\n", job->job_id(), job->cmd().characters());

    if (killpg(job->pgid(), SIGCONT) < 0) {
        perror("killpg");
        return 1;
    }

    int return_value = 0;

    auto current_pid = getpid();
    auto current_pgid = getpgid(current_pid);

    setpgid(job->pid(), job->pgid());
    tcsetpgrp(0, job->pgid());
    SpawnedProcess process { job->cmd(), job->pid() };

    do {
        if (wait_for_pid(process, true, return_value) == IterationDecision::Break)
            break;
    } while (errno == EINTR);

    setpgid(current_pid, current_pgid);
    tcsetpgrp(0, current_pgid);

    return return_value;
}

int Shell::builtin_disown(int argc, const char** argv)
{
    Vector<const char*> str_job_ids;

    Core::ArgsParser parser;
    parser.add_positional_argument(str_job_ids, "Id of the jobs to disown (omit for current job)", "job_ids", Core::ArgsParser::Required::No);

    if (!parser.parse(argc, const_cast<char**>(argv), false))
        return 1;

    Vector<size_t> job_ids;
    for (auto& job_id : str_job_ids) {
        auto id = StringView(job_id).to_uint();
        if (id.has_value())
            job_ids.append(id.value());
        else
            printf("Invalid job id: %s\n", job_id);
    }

    if (job_ids.is_empty())
        job_ids.append(jobs.size() - 1);

    Vector<size_t> keys_of_jobs_to_disown;

    for (auto id : job_ids) {
        bool found = false;
        for (auto& entry : jobs) {
            if (entry.value->job_id() == id) {
                keys_of_jobs_to_disown.append(entry.key);
                found = true;
                break;
            }
        }
        if (!found) {
            printf("job with id %zu not found\n", id);
        }
    }
    if (keys_of_jobs_to_disown.is_empty()) {
        if (str_job_ids.is_empty()) {
            printf("disown: no current job\n");
        }
        // An error message has already been printed about the nonexistence of each listed job.
        return 1;
    }
    for (auto job_index : keys_of_jobs_to_disown) {
        auto job = jobs.get(job_index).value();

        job->deactivate();

        if (!job->is_running_in_background())
            printf("disown warning: job %llu is currently not running, 'kill -%d %d' to make it continue\n", job->job_id(), SIGCONT, job->pid());

        jobs.remove(job_index);
    }

    return 0;
}

int Shell::builtin_history(int, const char**)
{
    for (size_t i = 0; i < editor->history().size(); ++i) {
        printf("%6zu  %s\n", i, editor->history()[i].characters());
    }
    return 0;
}

int Shell::builtin_jobs(int argc, const char** argv)
{
    bool list = false, show_pid = false;

    Core::ArgsParser parser;
    parser.add_option(list, "List all information about jobs", "list", 'l');
    parser.add_option(show_pid, "Display the PID of the jobs", "pid", 'p');

    if (!parser.parse(argc, const_cast<char**>(argv), false))
        return 1;

    enum {
        Basic,
        OnlyPID,
        ListAll,
    } mode { Basic };

    if (show_pid)
        mode = OnlyPID;

    if (list)
        mode = ListAll;

    for (auto& job : jobs) {
        auto pid = job.value->pid();
        int wstatus;
        auto rc = waitpid(pid, &wstatus, WNOHANG);
        if (rc == -1) {
            perror("waitpid");
            return 1;
        }
        auto status = "running";

        if (rc != 0) {
            if (WIFEXITED(wstatus))
                status = "exited";

            if (WIFSTOPPED(wstatus))
                status = "stopped";

            if (WIFSIGNALED(wstatus))
                status = "signaled";
        }

        char background_indicator = '-';

        if (job.value->is_running_in_background())
            background_indicator = '+';

        switch (mode) {
        case Basic:
            printf("[%llu] %c %s %s\n", job.value->job_id(), background_indicator, status, job.value->cmd().characters());
            break;
        case OnlyPID:
            printf("[%llu] %c %d %s %s\n", job.value->job_id(), background_indicator, pid, status, job.value->cmd().characters());
            break;
        case ListAll:
            printf("[%llu] %c %d %d %s %s\n", job.value->job_id(), background_indicator, pid, job.value->pgid(), status, job.value->cmd().characters());
            break;
        }
    }

    return 0;
}

int Shell::builtin_popd(int argc, const char** argv)
{
    if (directory_stack.size() <= 1) {
        fprintf(stderr, "Shell: popd: directory stack empty\n");
        return 1;
    }

    bool should_not_switch = false;
    String path = directory_stack.take_last();

    Core::ArgsParser parser;
    parser.add_option(should_not_switch, "Do not switch dirs", "no-switch", 'n');

    if (!parser.parse(argc, const_cast<char**>(argv), false))
        return 1;

    bool should_switch = !should_not_switch;

    // When no arguments are given, popd removes the top directory from the stack and performs a cd to the new top directory.
    if (argc == 1) {
        int rc = chdir(path.characters());
        if (rc < 0) {
            fprintf(stderr, "chdir(%s) failed: %s\n", path.characters(), strerror(errno));
            return 1;
        }

        cwd = path;
        return 0;
    }

    LexicalPath lexical_path(path.characters());
    if (!lexical_path.is_valid()) {
        fprintf(stderr, "LexicalPath failed to canonicalize '%s'\n", path.characters());
        return 1;
    }

    const char* real_path = lexical_path.string().characters();

    struct stat st;
    int rc = stat(real_path, &st);
    if (rc < 0) {
        fprintf(stderr, "stat(%s) failed: %s\n", real_path, strerror(errno));
        return 1;
    }

    if (!S_ISDIR(st.st_mode)) {
        fprintf(stderr, "Not a directory: %s\n", real_path);
        return 1;
    }

    if (should_switch) {
        int rc = chdir(real_path);
        if (rc < 0) {
            fprintf(stderr, "chdir(%s) failed: %s\n", real_path, strerror(errno));
            return 1;
        }

        cwd = lexical_path.string();
    }

    return 0;
}

int Shell::builtin_pushd(int argc, const char** argv)
{
    StringBuilder path_builder;
    bool should_switch = true;

    // From the BASH reference manual: https://www.gnu.org/software/bash/manual/html_node/Directory-Stack-Builtins.html
    // With no arguments, pushd exchanges the top two directories and makes the new top the current directory.
    if (argc == 1) {
        if (directory_stack.size() < 2) {
            fprintf(stderr, "pushd: no other directory\n");
            return 1;
        }

        String dir1 = directory_stack.take_first();
        String dir2 = directory_stack.take_first();
        directory_stack.insert(0, dir2);
        directory_stack.insert(1, dir1);

        int rc = chdir(dir2.characters());
        if (rc < 0) {
            fprintf(stderr, "chdir(%s) failed: %s\n", dir2.characters(), strerror(errno));
            return 1;
        }

        cwd = dir2;

        return 0;
    }

    // Let's assume the user's typed in 'pushd <dir>'
    if (argc == 2) {
        directory_stack.append(cwd.characters());
        if (argv[1][0] == '/') {
            path_builder.append(argv[1]);
        } else {
            path_builder.appendf("%s/%s", cwd.characters(), argv[1]);
        }
    } else if (argc == 3) {
        directory_stack.append(cwd.characters());
        for (int i = 1; i < argc; i++) {
            const char* arg = argv[i];

            if (arg[0] != '-') {
                if (arg[0] == '/') {
                    path_builder.append(arg);
                } else
                    path_builder.appendf("%s/%s", cwd.characters(), arg);
            }

            if (!strcmp(arg, "-n"))
                should_switch = false;
        }
    }

    LexicalPath lexical_path(path_builder.to_string());
    if (!lexical_path.is_valid()) {
        fprintf(stderr, "LexicalPath failed to canonicalize '%s'\n", path_builder.to_string().characters());
        return 1;
    }

    const char* real_path = lexical_path.string().characters();

    struct stat st;
    int rc = stat(real_path, &st);
    if (rc < 0) {
        fprintf(stderr, "stat(%s) failed: %s\n", real_path, strerror(errno));
        return 1;
    }

    if (!S_ISDIR(st.st_mode)) {
        fprintf(stderr, "Not a directory: %s\n", real_path);
        return 1;
    }

    if (should_switch) {
        int rc = chdir(real_path);
        if (rc < 0) {
            fprintf(stderr, "chdir(%s) failed: %s\n", real_path, strerror(errno));
            return 1;
        }

        cwd = lexical_path.string();
    }

    return 0;
}

int Shell::builtin_pwd(int, const char**)
{
    print_path(cwd);
    fputc('\n', stdout);
    return 0;
}

int Shell::builtin_time(int argc, const char** argv)
{
    Vector<const char*> args;

    Core::ArgsParser parser;
    parser.add_positional_argument(args, "Command to execute with arguments", "command", Core::ArgsParser::Required::Yes);

    if (!parser.parse(argc, const_cast<char**>(argv), false))
        return 1;

    StringBuilder builder;
    builder.join(' ', args);

    Core::ElapsedTimer timer;
    timer.start();
    auto exit_code = run_command(builder.string_view());
    if (!exit_code.has_value()) {
        printf("Shell: Incomplete command: %s\n", builder.to_string().characters());
        exit_code = 1;
    }
    printf("Time: %d ms\n", timer.elapsed());
    return exit_code.value();
}

int Shell::builtin_umask(int argc, const char** argv)
{
    const char* mask_text = nullptr;

    Core::ArgsParser parser;
    parser.add_positional_argument(mask_text, "New mask (omit to get current mask)", "octal-mask", Core::ArgsParser::Required::No);

    if (!parser.parse(argc, const_cast<char**>(argv), false))
        return 1;

    if (!mask_text) {
        mode_t old_mask = umask(0);
        printf("%#o\n", old_mask);
        umask(old_mask);
        return 0;
    }

    unsigned mask;
    int matches = sscanf(mask_text, "%o", &mask);
    if (matches == 1) {
        umask(mask);
        return 0;
    }

    return 0;
}

int Shell::builtin_unset(int argc, const char** argv)
{
    Vector<const char*> vars;

    Core::ArgsParser parser;
    parser.add_positional_argument(vars, "List of variables", "variables", Core::ArgsParser::Required::Yes);

    if (!parser.parse(argc, const_cast<char**>(argv), false))
        return 1;

    for (auto& value : vars)
        unsetenv(value);

    return 0;
}

bool Shell::run_builtin(int argc, const char** argv, int& retval)
{
    if (argc == 0)
        return false;

    StringView name { argv[0] };

#define __ENUMERATE_SHELL_BUILTIN(builtin)      \
    if (name == #builtin) {                     \
        retval = builtin_##builtin(argc, argv); \
        return true;                            \
    }

    ENUMERATE_SHELL_BUILTINS();

#undef __ENUMERATE_SHELL_BUILTIN

    return false;
}

String Shell::expand_tilde(const String& expression)
{
    ASSERT(expression.starts_with('~'));

    StringBuilder login_name;
    size_t first_slash_index = expression.length();
    for (size_t i = 1; i < expression.length(); ++i) {
        if (expression[i] == '/') {
            first_slash_index = i;
            break;
        }
        login_name.append(expression[i]);
    }

    StringBuilder path;
    for (size_t i = first_slash_index; i < expression.length(); ++i)
        path.append(expression[i]);

    if (login_name.is_empty()) {
        const char* home = getenv("HOME");
        if (!home) {
            auto passwd = getpwuid(getuid());
            ASSERT(passwd && passwd->pw_dir);
            return String::format("%s/%s", passwd->pw_dir, path.to_string().characters());
        }
        return String::format("%s/%s", home, path.to_string().characters());
    }

    auto passwd = getpwnam(login_name.to_string().characters());
    if (!passwd)
        return expression;
    ASSERT(passwd->pw_dir);

    return String::format("%s/%s", passwd->pw_dir, path.to_string().characters());
}

bool Shell::is_glob(const StringView& s)
{
    for (size_t i = 0; i < s.length(); i++) {
        char c = s.characters_without_null_termination()[i];
        if (c == '*' || c == '?')
            return true;
    }
    return false;
}

Vector<StringView> Shell::split_path(const StringView& path)
{
    Vector<StringView> parts;

    size_t substart = 0;
    for (size_t i = 0; i < path.length(); i++) {
        char ch = path.characters_without_null_termination()[i];
        if (ch != '/')
            continue;
        size_t sublen = i - substart;
        if (sublen != 0)
            parts.append(path.substring_view(substart, sublen));
        parts.append(path.substring_view(i, 1));
        substart = i + 1;
    }

    size_t taillen = path.length() - substart;
    if (taillen != 0)
        parts.append(path.substring_view(substart, taillen));

    return parts;
}

Vector<String> Shell::expand_globs(const StringView& path, const StringView& base)
{
    auto parts = split_path(path);

    StringBuilder builder;
    builder.append(base);
    Vector<String> res;

    for (size_t i = 0; i < parts.size(); ++i) {
        auto& part = parts[i];
        if (!is_glob(part)) {
            builder.append(part);
            continue;
        }

        // Found a glob.
        String new_base = builder.to_string();
        StringView new_base_v = new_base;
        if (new_base_v.is_empty())
            new_base_v = ".";
        Core::DirIterator di(new_base_v, Core::DirIterator::SkipParentAndBaseDir);

        if (di.has_error()) {
            return res;
        }

        while (di.has_next()) {
            String name = di.next_path();

            // Dotfiles have to be explicitly requested
            if (name[0] == '.' && part[0] != '.')
                continue;

            if (name.matches(part, CaseSensitivity::CaseSensitive)) {

                StringBuilder nested_base;
                nested_base.append(new_base);
                nested_base.append(name);

                StringView remaining_path = path.substring_view_starting_after_substring(part);
                Vector<String> nested_res = expand_globs(remaining_path, nested_base.to_string());
                for (auto& s : nested_res)
                    res.append(s);
            }
        }
        return res;
    }

    // Found no globs.
    String new_path = builder.to_string();
    if (access(new_path.characters(), F_OK) == 0)
        res.append(new_path);
    return res;
}

Vector<String> Shell::expand_parameters(const StringView& param) const
{
    if (!param.starts_with('$'))
        return { param };

    String variable_name = String(param.substring_view(1, param.length() - 1));
    if (variable_name == "?")
        return { String::number(last_return_code) };
    else if (variable_name == "$")
        return { String::number(getpid()) };

    char* env_value = getenv(variable_name.characters());
    if (env_value == nullptr)
        return { "" };

    Vector<String> res;
    String str_env_value = String(env_value);
    const auto& split_text = str_env_value.split_view(' ');
    for (auto& part : split_text)
        res.append(part);
    return res;
}

Vector<String> Shell::process_arguments(const Vector<Token>& args)
{
    Vector<String> argv_string;
    for (auto& arg : args) {
        if (arg.type == Token::Comment)
            continue;

        // This will return the text passed in if it wasn't a variable
        // This lets us just loop over its values
        auto expanded_parameters = expand_parameters(arg.text);

        for (auto& exp_arg : expanded_parameters) {
            if (exp_arg.starts_with('~'))
                exp_arg = expand_tilde(exp_arg);

            auto expanded_globs = expand_globs(exp_arg, "");
            for (auto& path : expanded_globs)
                argv_string.append(path);

            if (expanded_globs.is_empty())
                argv_string.append(exp_arg);
        }
    }

    return argv_string;
}

ContinuationRequest Shell::is_complete(const Vector<Command>& commands)
{
    // check if the last command ends with a pipe, or an unterminated string
    auto& last_command = commands.last();
    auto& subcommands = last_command.subcommands;
    if (subcommands.size() == 0)
        return ContinuationRequest::Nothing;

    auto& last_subcommand = subcommands.last();

    if (!last_subcommand.redirections.find([](auto& redirection) { return redirection.type == Redirection::Pipe; }).is_end())
        return ContinuationRequest::Pipe;

    if (!last_subcommand.args.find([](auto& token) { return token.type == Token::UnterminatedSingleQuoted; }).is_end())
        return ContinuationRequest::SingleQuotedString;

    if (!last_subcommand.args.find([](auto& token) { return token.type == Token::UnterminatedDoubleQuoted; }).is_end())
        return ContinuationRequest::DoubleQuotedString;

    return ContinuationRequest::Nothing;
}

IterationDecision Shell::wait_for_pid(const Shell::SpawnedProcess& process, bool is_first_command_in_chain, int& return_value)
{
    if (is_first_command_in_chain)
        m_waiting_for_pid = process.pid;

    int wstatus = 0;
    int rc = waitpid(process.pid, &wstatus, WSTOPPED);
    auto errno_save = errno;

    if (is_first_command_in_chain)
        m_waiting_for_pid = -1;

    errno = errno_save;
    if (rc < 0 && errno != EINTR) {
        if (errno != ECHILD)
            perror("waitpid");
        return IterationDecision::Break;
    }

    const Job* job = nullptr;
    u64 job_id = 0;
    auto maybe_job = jobs.get(process.pid);
    if (maybe_job.has_value()) {
        job = maybe_job.value();
        job_id = job->job_id();
    }

    if (WIFEXITED(wstatus)) {
        if (WEXITSTATUS(wstatus) != 0)
            dbg() << "Shell: " << process.name << ":" << process.pid << " exited with status " << WEXITSTATUS(wstatus);

        return_value = WEXITSTATUS(wstatus);

        if (job) {
            auto* mutable_job = const_cast<Job*>(job);
            mutable_job->set_has_exit(return_value);
            Core::EventLoop::current().post_event(*this, make<Core::CustomEvent>(ChildExited, mutable_job));
        }
        return IterationDecision::Break;
    }

    if (WIFSTOPPED(wstatus)) {
        fprintf(stderr, "Shell: [%llu] %s(%d) %s\n", job_id, process.name.characters(), process.pid, strsignal(WSTOPSIG(wstatus)));
        return IterationDecision::Continue;
    }

    if (WIFSIGNALED(wstatus)) {
        printf("Shell: [%llu] %s(%d) exited due to signal '%s'\n", job_id, process.name.characters(), process.pid, strsignal(WTERMSIG(wstatus)));
    } else {
        printf("Shell: [%llu] %s(%d) exited abnormally\n", job_id, process.name.characters(), process.pid);
    }
    if (job) {
        auto* mutable_job = const_cast<Job*>(job);
        mutable_job->set_has_exit(-1);
        Core::EventLoop::current().post_event(*this, make<Core::CustomEvent>(ChildExited, mutable_job));
    }
    return IterationDecision::Break;
}

ExitCodeOrContinuationRequest Shell::run_command(const StringView& cmd)
{
    if (cmd.is_empty())
        return 0;

    if (cmd.starts_with("#"))
        return 0;

    auto commands = Parser(cmd).parse();

    if (!commands.size())
        return 1;

    auto needs_more = is_complete(commands);
    if (needs_more != ExitCodeOrContinuationRequest::Nothing)
        return needs_more;

#ifdef SH_DEBUG
    for (auto& command : commands) {
        for (size_t i = 0; i < command.subcommands.size(); ++i) {
            for (size_t j = 0; j < i; ++j)
                dbgprintf("    ");
            for (auto& arg : command.subcommands[i].args) {
                switch (arg.type) {
                case Token::Bare:
                    dbgprintf("<%s> ", arg.text.characters());
                    break;
                case Token::SingleQuoted:
                    dbgprintf("'<%s>' ", arg.text.characters());
                    break;
                case Token::DoubleQuoted:
                    dbgprintf("\"<%s>\" ", arg.text.characters());
                    break;
                case Token::UnterminatedSingleQuoted:
                    dbgprintf("\'<%s> ", arg.text.characters());
                    break;
                case Token::UnterminatedDoubleQuoted:
                    dbgprintf("\"<%s> ", arg.text.characters());
                    break;
                case Token::Special:
                    dbgprintf("<%s> ", arg.text.characters());
                    break;
                case Token::Comment:
                    dbgprintf("<%s> ", arg.text.characters());
                    break;
                }
            }

            dbgprintf("\n");
            for (auto& redirecton : command.subcommands[i].redirections) {
                for (size_t j = 0; j < i; ++j)
                    dbgprintf("    ");
                dbgprintf("  ");
                switch (redirecton.type) {
                case Redirection::Pipe:
                    dbgprintf("Pipe\n");
                    break;
                case Redirection::FileRead:
                    dbgprintf("fd:%d = FileRead: %s\n", redirecton.fd, redirecton.path.text.characters());
                    break;
                case Redirection::FileWrite:
                    dbgprintf("fd:%d = FileWrite: %s\n", redirecton.fd, redirecton.path.text.characters());
                    break;
                case Redirection::FileWriteAppend:
                    dbgprintf("fd:%d = FileWriteAppend: %s\n", redirecton.fd, redirecton.path.text.characters());
                    break;
                default:
                    break;
                }
            }
        }
        if (auto attributes = command.attributes) {
            dbgprintf("\n ");
            if (attributes & Attributes::InBackground)
                dbgprintf("InBackground ");
            if (attributes & Attributes::ShortCircuitOnFailure)
                dbgprintf("ShortCircuitOnFailure ");
        }
        dbgprintf("\n");
    }
#endif

    struct termios trm;
    tcgetattr(0, &trm);

    int return_value = 0;
    bool fail_short_circuits = false;

    for (auto& command : commands) {
        if (fail_short_circuits) {
            if (command.attributes & Attributes::ShortCircuitOnFailure)
                continue;

            // Do not fail any command after this one, as we've reached the end of a short-circuit chain,
            // e.g. foo && bar && baz ; foobar
            //                    ^ we reached this command.
            fail_short_circuits = false;
            continue;
        }

        if (command.subcommands.is_empty())
            continue;

        FileDescriptionCollector fds;

        for (size_t i = 0; i < command.subcommands.size(); ++i) {
            auto& subcommand = command.subcommands[i];
            for (auto& redirection : subcommand.redirections) {
                switch (redirection.type) {
                case Redirection::Pipe: {
                    int pipefd[2];
                    int rc = pipe(pipefd);
                    if (rc < 0) {
                        perror("pipe");
                        return 1;
                    }
                    subcommand.rewirings.append({ STDOUT_FILENO, pipefd[1] });
                    auto& next_command = command.subcommands[i + 1];
                    next_command.rewirings.append({ STDIN_FILENO, pipefd[0] });
                    fds.add(pipefd[0]);
                    fds.add(pipefd[1]);
                    break;
                }
                case Redirection::FileWriteAppend: {
                    int fd = open(redirection.path.text.characters(), O_WRONLY | O_CREAT | O_APPEND, 0666);
                    if (fd < 0) {
                        perror("open");
                        return 1;
                    }
                    subcommand.rewirings.append({ redirection.fd, fd });
                    fds.add(fd);
                    break;
                }
                case Redirection::FileWrite: {
                    int fd = open(redirection.path.text.characters(), O_WRONLY | O_CREAT | O_TRUNC, 0666);
                    if (fd < 0) {
                        perror("open");
                        return 1;
                    }
                    subcommand.rewirings.append({ redirection.fd, fd });
                    fds.add(fd);
                    break;
                }
                case Redirection::FileRead: {
                    int fd = open(redirection.path.text.characters(), O_RDONLY);
                    if (fd < 0) {
                        perror("open");
                        return 1;
                    }
                    subcommand.rewirings.append({ redirection.fd, fd });
                    fds.add(fd);
                    break;
                }
                }
            }
        }

        Vector<SpawnedProcess> children;

        for (size_t i = 0; i < command.subcommands.size(); ++i) {
            auto& subcommand = command.subcommands[i];
            Vector<String> argv_string = process_arguments(subcommand.args);
            Vector<const char*> argv;
            argv.ensure_capacity(argv_string.size());
            for (const auto& s : argv_string) {
                argv.append(s.characters());
            }
            argv.append(nullptr);

#ifdef SH_DEBUG
            for (auto& arg : argv) {
                dbgprintf("<%s> ", arg);
            }
            dbgprintf("\n");
#endif

            int retval = 0;
            if (run_builtin(argv.size() - 1, argv.data(), retval))
                return retval;

            pid_t child = fork();
            if (!child) {
                setpgid(0, 0);
                tcsetpgrp(0, getpid());
                tcsetattr(0, TCSANOW, &default_termios);
                for (auto& rewiring : subcommand.rewirings) {
#ifdef SH_DEBUG
                    dbgprintf("in %s<%d>, dup2(%d, %d)\n", argv[0], getpid(), rewiring.rewire_fd, rewiring.fd);
#endif
                    int rc = dup2(rewiring.rewire_fd, rewiring.fd);
                    if (rc < 0) {
                        perror("dup2");
                        return 1;
                    }
                }

                fds.collect();

                int rc = execvp(argv[0], const_cast<char* const*>(argv.data()));
                if (rc < 0) {
                    if (errno == ENOENT) {
                        int shebang_fd = open(argv[0], O_RDONLY);
                        auto close_argv = ScopeGuard([shebang_fd]() { if (shebang_fd >= 0)  close(shebang_fd); });
                        char shebang[256] {};
                        ssize_t num_read = -1;
                        if ((shebang_fd >= 0) && ((num_read = read(shebang_fd, shebang, sizeof(shebang))) >= 2) && (StringView(shebang).starts_with("#!"))) {
                            StringView shebang_path_view(&shebang[2], num_read - 2);
                            Optional<size_t> newline_pos = shebang_path_view.find_first_of("\n\r");
                            shebang[newline_pos.has_value() ? (newline_pos.value() + 2) : num_read] = '\0';
                            fprintf(stderr, "%s: Invalid interpreter \"%s\": %s\n", argv[0], &shebang[2], strerror(ENOENT));
                        } else
                            fprintf(stderr, "%s: Command not found.\n", argv[0]);
                    } else {
                        int saved_errno = errno;
                        struct stat st;
                        if (stat(argv[0], &st) == 0 && S_ISDIR(st.st_mode)) {
                            fprintf(stderr, "Shell: %s: Is a directory\n", argv[0]);
                            _exit(126);
                        }
                        fprintf(stderr, "execvp(%s): %s\n", argv[0], strerror(saved_errno));
                    }
                    _exit(126);
                }
                ASSERT_NOT_REACHED();
            }
            children.append({ argv[0], child });

            StringBuilder cmd;
            cmd.join(" ", argv_string);

            auto job = make<Job>(child, (unsigned)child, cmd.build(), find_last_job_id() + 1);
            jobs.set((u64)child, move(job));
        }

#ifdef SH_DEBUG
        dbgprintf("Closing fds in shell process:\n");
#endif
        fds.collect();

#ifdef SH_DEBUG
        dbgprintf("Now we gotta wait on children:\n");
        for (auto& child : children)
            dbgprintf("  %d (%s)\n", child.pid, child.name.characters());
#endif

        if (command.attributes & Attributes::InBackground) {
            // Set the jobs as running in background and continue without waiting.
            for (auto& child : children)
                const_cast<Job*>(jobs.get(child.pid).value())->set_running_in_background(true);

            continue;
        }

        for (size_t i = 0; i < children.size(); ++i) {
            auto& child = children[i];
            dbg() << "Now waiting for " << child.name << " (" << child.pid << ")";
            do {
                if (wait_for_pid(child, i != children.size() - 1, return_value) == IterationDecision::Break)
                    break;
            } while (errno == EINTR);
        }

        if (command.attributes & Attributes::ShortCircuitOnFailure) {
            if (return_value != 0) {
                fail_short_circuits = true;
            }
        }
    }

    last_return_code = return_value;

    // FIXME: Should I really have to tcsetpgrp() after my child has exited?
    //        Is the terminal controlling pgrp really still the PGID of the dead process?
    tcsetpgrp(0, getpid());
    tcsetattr(0, TCSANOW, &trm);

    // Clear the exit flag after any non-exit command has been executed.
    m_should_ignore_jobs_on_next_exit = false;

    return return_value;
}

String Shell::get_history_path()
{
    StringBuilder builder;
    builder.append(home);
    builder.append("/.history");
    return builder.to_string();
}

void Shell::load_history()
{
    auto history_file = Core::File::construct(get_history_path());
    if (!history_file->open(Core::IODevice::ReadOnly))
        return;
    while (history_file->can_read_line()) {
        auto b = history_file->read_line(1024);
        // skip the newline and terminating bytes
        editor->add_to_history(String(reinterpret_cast<const char*>(b.data()), b.size() - 2));
    }
}

void Shell::save_history()
{
    auto file_or_error = Core::File::open(get_history_path(), Core::IODevice::WriteOnly, 0600);
    if (file_or_error.is_error())
        return;
    auto& file = *file_or_error.value();
    for (const auto& line : editor->history()) {
        file.write(line);
        file.write("\n");
    }
}

String Shell::escape_token(const String& token)
{
    StringBuilder builder;

    for (auto c : token) {
        switch (c) {
        case '\'':
        case '"':
        case '$':
        case '|':
        case '>':
        case '<':
        case '&':
        case '\\':
        case ' ':
            builder.append('\\');
            break;
        default:
            break;
        }
        builder.append(c);
    }

    return builder.build();
}

String Shell::unescape_token(const String& token)
{
    StringBuilder builder;

    enum {
        Free,
        Escaped
    } state { Free };

    for (auto c : token) {
        switch (state) {
        case Escaped:
            builder.append(c);
            state = Free;
            break;
        case Free:
            if (c == '\\')
                state = Escaped;
            else
                builder.append(c);
            break;
        }
    }

    if (state == Escaped)
        builder.append('\\');

    return builder.build();
}

void Shell::cache_path()
{
    if (!cached_path.is_empty())
        cached_path.clear_with_capacity();

    String path = getenv("PATH");
    if (path.is_empty())
        return;

    auto directories = path.split(':');
    for (const auto& directory : directories) {
        Core::DirIterator programs(directory.characters(), Core::DirIterator::SkipDots);
        while (programs.has_next()) {
            auto program = programs.next_path();
            String program_path = String::format("%s/%s", directory.characters(), program.characters());
            if (access(program_path.characters(), X_OK) == 0)
                cached_path.append(escape_token(program.characters()));
        }
    }

    // add shell builtins to the cache
    for (const auto& builtin_name : builtin_names)
        cached_path.append(escape_token(builtin_name));

    quick_sort(cached_path);
}

void Shell::highlight(Line::Editor& editor) const
{
    StringBuilder builder;
    bool is_offset_by_string_start = false;
    if (m_should_continue == ExitCodeOrContinuationRequest::DoubleQuotedString) {
        builder.append('"');
        is_offset_by_string_start = true;
    }
    if (m_should_continue == ExitCodeOrContinuationRequest::SingleQuotedString) {
        builder.append('\'');
        is_offset_by_string_start = true;
    }
    builder.append(editor.line());
    auto commands = Parser { builder.string_view() }.parse();
    auto first_command { true };
    for (auto& command : commands) {
        for (auto& subcommand : command.subcommands) {
            auto& redirections = subcommand.redirections;
            for (auto& redirection : redirections) {
                if (redirection.type == Redirection::Pipe)
                    continue;
                if (redirection.path.length == 0)
                    continue;
                Line::Style redirection_style { Line::Style::Foreground(0x87, 0x9b, 0xcd) }; // 25% darkened periwinkle :)
                auto end = redirection.path.end;
                auto redirection_op_start = redirection.redirection_op_start;
                if (is_offset_by_string_start) {
                    end--;
                    redirection_op_start--;
                }

                editor.stylize({ redirection_op_start, end }, redirection_style);
            }
            auto first { true };
            for (auto& arg : subcommand.args) {
                auto start = arg.end - arg.length;

                if (arg.type == Token::Comment) {
                    editor.stylize({ start, arg.end }, { Line::Style::Foreground(150, 150, 150) }); // light gray
                    continue;
                }

                if (m_should_continue == ExitCodeOrContinuationRequest::DoubleQuotedString || m_should_continue == ExitCodeOrContinuationRequest::SingleQuotedString) {
                    if (!first_command)
                        --start;
                    --arg.end;
                }
                if (first) {
                    first = false;
                    // only treat this as a command name if we're not continuing strings
                    if (!first_command || (m_should_continue == ExitCodeOrContinuationRequest::Nothing || m_should_continue == ExitCodeOrContinuationRequest::Pipe)) {
                        editor.stylize({ start, arg.end }, { Line::Style::Bold });
                        first_command = false;
                        continue;
                    }
                    first_command = false;
                }

                if (arg.type == Token::SingleQuoted || arg.type == Token::UnterminatedSingleQuoted) {
                    editor.stylize({ start - 1, arg.end + (arg.type != Token::UnterminatedSingleQuoted) }, { Line::Style::Foreground(Line::Style::XtermColor::Yellow) });
                    continue;
                }

                if (arg.type == Token::DoubleQuoted || arg.type == Token::UnterminatedDoubleQuoted) {
                    editor.stylize({ start - 1, arg.end + (arg.type != Token::UnterminatedDoubleQuoted) }, { Line::Style::Foreground(Line::Style::XtermColor::Yellow) });
                    if constexpr (HighlightVariablesInsideStrings)
                        goto highlight_variables;
                    else
                        continue;
                }

                if (is_glob(arg.text)) {
                    editor.stylize({ start, arg.end }, { Line::Style::Foreground(59, 142, 234) }); // bright-ish blue
                    continue;
                }

                if (arg.text.starts_with("--")) {
                    if (arg.length == 2)
                        editor.stylize({ start, arg.end }, { Line::Style::Foreground(Line::Style::XtermColor::Green) });
                    else
                        editor.stylize({ start, arg.end }, { Line::Style::Foreground(Line::Style::XtermColor::Cyan) });
                } else if (arg.text.starts_with("-") && arg.length > 1) {
                    editor.stylize({ start, arg.end }, { Line::Style::Foreground(Line::Style::XtermColor::Cyan) });
                }

            highlight_variables:;

                size_t slice_index = 0;
                Optional<size_t> maybe_index;
                while (slice_index < arg.length) {
                    maybe_index = arg.text.substring_view(slice_index, arg.length - slice_index).find_first_of('$');
                    if (!maybe_index.has_value())
                        break;
                    auto index = maybe_index.value() + 1;
                    auto end_index = index;
                    if (index >= arg.length)
                        break;
                    for (; end_index < arg.length; ++end_index) {
                        if (!is_word_character(arg.text[end_index]))
                            break;
                    }
                    editor.stylize({ index + start - 1, end_index + start }, { Line::Style::Foreground(214, 112, 214) });
                    slice_index = end_index + 1;
                }
            }
        }
    }
}

Vector<Line::CompletionSuggestion> Shell::complete(const Line::Editor& editor)
{
    auto line = editor.line(editor.cursor());

    Parser parser(line);

    auto commands = parser.parse();

    if (commands.size() == 0)
        return {};

    // Get the last token and whether it's the first in its subcommand.
    String token;
    bool is_first_in_subcommand = false;
    auto& subcommand = commands.last().subcommands;
    String file_token_trail = " ";
    String directory_token_trail = "/";

    if (subcommand.size() == 0) {
        // foo bar; <tab>
        token = "";
        is_first_in_subcommand = true;
    } else {
        auto& last_command = subcommand.last();
        if (!last_command.redirections.is_empty() && last_command.redirections.last().type != Redirection::Pipe) {
            // foo > bar<tab>
            const auto& redirection = last_command.redirections.last();
            const auto& path = redirection.path;

            if (path.end != line.length())
                return {};

            token = path.text;
            is_first_in_subcommand = false;
            if (path.type == Token::UnterminatedDoubleQuoted)
                file_token_trail = "\"";
            else if (path.type == Token::UnterminatedSingleQuoted)
                file_token_trail = "'";
        } else {
            if (last_command.args.size() == 0) {
                // foo bar | <tab>
                token = "";
                is_first_in_subcommand = true;
            } else {
                auto& args = last_command.args;
                if (args.last().type == Token::Comment) // we cannot complete comments
                    return {};

                if (args.last().end != line.length()) {
                    // There was a token separator at the end
                    is_first_in_subcommand = false;
                    token = "";
                } else {
                    is_first_in_subcommand = args.size() == 1;
                    token = last_command.args.last().text;
                }
            }
        }
    }

    Vector<Line::CompletionSuggestion> suggestions;

    bool should_suggest_only_executables = false;

    if (is_first_in_subcommand) {
        auto match = binary_search(cached_path.data(), cached_path.size(), token, [](const String& token, const String& program) -> int {
            return strncmp(token.characters(), program.characters(), token.length());
        });

        if (match) {
            String completion = *match;
            editor.suggest(escape_token(token).length(), 0);

            // Now that we have a program name starting with our token, we look at
            // other program names starting with our token and cut off any mismatching
            // characters.

            int index = match - cached_path.data();
            for (int i = index - 1; i >= 0 && cached_path[i].starts_with(token); --i) {
                suggestions.append({ cached_path[i], " " });
            }
            for (size_t i = index + 1; i < cached_path.size() && cached_path[i].starts_with(token); ++i) {
                suggestions.append({ cached_path[i], " " });
            }
            suggestions.append({ cached_path[index], " " });

            return suggestions;
        }

        // fallthrough to suggesting local files, but make sure to only suggest executables
        should_suggest_only_executables = true;
    }

    String path;
    String original_token = token;

    ssize_t last_slash = token.length() - 1;
    while (last_slash >= 0 && token[last_slash] != '/')
        --last_slash;

    if (last_slash >= 0) {
        // Split on the last slash. We'll use the first part as the directory
        // to search and the second part as the token to complete.
        path = token.substring(0, last_slash + 1);
        if (path[0] != '/')
            path = String::format("%s/%s", cwd.characters(), path.characters());
        path = LexicalPath::canonicalized_path(path);
        token = token.substring(last_slash + 1, token.length() - last_slash - 1);
    } else {
        // We have no slashes, so the directory to search is the current
        // directory and the token to complete is just the original token.
        path = cwd;
    }

    // the invariant part of the token is actually just the last segment
    // e. in `cd /foo/bar', 'bar' is the invariant
    //      since we are not suggesting anything starting with
    //      `/foo/', but rather just `bar...'
    auto token_length = escape_token(token).length();
    editor.suggest(token_length, original_token.length() - token_length);

    // only suggest dot-files if path starts with a dot
    Core::DirIterator files(path,
        token.starts_with('.') ? Core::DirIterator::SkipParentAndBaseDir : Core::DirIterator::SkipDots);

    while (files.has_next()) {
        auto file = files.next_path();
        if (file.starts_with(token)) {
            struct stat program_status;
            String file_path = String::format("%s/%s", path.characters(), file.characters());
            int stat_error = stat(file_path.characters(), &program_status);
            if (!stat_error) {
                if (S_ISDIR(program_status.st_mode)) {
                    if (!should_suggest_only_executables)
                        suggestions.append({ escape_token(file), directory_token_trail, { Line::Style::Hyperlink(String::format("file://%s", file_path.characters())), Line::Style::Anchored } });
                } else {
                    suggestions.append({ escape_token(file), file_token_trail, { Line::Style::Hyperlink(String::format("file://%s", file_path.characters())), Line::Style::Anchored } });
                }
            }
        }
    }

    return suggestions;
}

bool Shell::read_single_line()
{
    auto line_result = editor->get_line(prompt());

    if (line_result.is_error()) {
        if (line_result.error() == Line::Editor::Error::Eof || line_result.error() == Line::Editor::Error::Empty) {
            // Pretend the user tried to execute builtin_exit()
            // but only if there's no continuation.
            if (m_should_continue == ContinuationRequest::Nothing) {
                m_complete_line_builder.clear();
                run_command("exit");
                return read_single_line();
            } else {
                // Ignore the Eof.
                return true;
            }
        } else {
            m_complete_line_builder.clear();
            m_should_continue = ContinuationRequest::Nothing;
            m_should_break_current_command = false;
            Core::EventLoop::current().quit(1);
            return false;
        }
    }

    auto& line = line_result.value();

    if (m_should_break_current_command) {
        m_complete_line_builder.clear();
        m_should_continue = ContinuationRequest::Nothing;
        m_should_break_current_command = false;
        return true;
    }

    if (line.is_empty())
        return true;

    if (!m_complete_line_builder.is_empty())
        m_complete_line_builder.append("\n");
    m_complete_line_builder.append(line);

    auto complete_or_exit_code = run_command(m_complete_line_builder.string_view());
    m_should_continue = complete_or_exit_code.continuation;

    if (!complete_or_exit_code.has_value())
        return true;

    editor->add_to_history(m_complete_line_builder.build());
    m_complete_line_builder.clear();
    return true;
}

void Shell::custom_event(Core::CustomEvent& event)
{
    if (event.custom_type() == ReadLine) {
        if (read_single_line())
            Core::EventLoop::current().post_event(*this, make<Core::CustomEvent>(ShellEventType::ReadLine));
        return;
    }

    if (event.custom_type() == ChildExited) {
        auto* job_ptr = event.data();
        if (job_ptr) {
            auto& job = *(Job*)job_ptr;
            if (job.is_running_in_background())
                fprintf(stderr, "Shell: Job %d(%s) exited\n", job.pid(), job.cmd().characters());
            jobs.remove(job.pid());
        }
        return;
    }

    event.ignore();
}

Shell::Shell()
{
    uid = getuid();
    tcsetpgrp(0, getpgrp());

    int rc = gethostname(hostname, Shell::HostNameSize);
    if (rc < 0)
        perror("gethostname");
    rc = ttyname_r(0, ttyname, Shell::TTYNameSize);
    if (rc < 0)
        perror("ttyname_r");

    {
        auto* cwd = getcwd(nullptr, 0);
        this->cwd = cwd;
        setenv("PWD", cwd, 1);
        free(cwd);
    }

    {
        auto* pw = getpwuid(getuid());
        if (pw) {
            username = pw->pw_name;
            home = pw->pw_dir;
            setenv("HOME", pw->pw_dir, 1);
        }
        endpwent();
    }

    directory_stack.append(cwd);
    load_history();
    cache_path();
}

Shell::~Shell()
{
    stop_all_jobs();
    save_history();
}

void Shell::stop_all_jobs()
{
    if (!jobs.is_empty()) {
        printf("Killing active jobs\n");
        for (auto& entry : jobs) {
            if (!entry.value->is_running_in_background()) {
#ifdef SH_DEBUG
                dbg() << "Job " << entry.value->pid() << " is not running in background";
#endif
                if (killpg(entry.value->pgid(), SIGCONT) < 0) {
                    perror("killpg(CONT)");
                }
            }

            if (killpg(entry.value->pgid(), SIGHUP) < 0) {
                perror("killpg(HUP)");
            }

            if (killpg(entry.value->pgid(), SIGTERM) < 0) {
                perror("killpg(TERM)");
            }
        }

        usleep(10000); // Wait for a bit before killing the job

        for (auto& entry : jobs) {
#ifdef SH_DEBUG
            dbg() << "Actively killing " << entry.value->pid() << "(" << entry.value->cmd() << ")";
#endif
            if (killpg(entry.value->pgid(), SIGKILL) < 0) {
                if (errno == ESRCH)
                    continue; // The process has exited all by itself.
                perror("killpg(KILL)");
            }
        }
    }
}

u64 Shell::find_last_job_id() const
{
    u64 job_id = 0;
    for (auto& entry : jobs) {
        if (entry.value->job_id() > job_id)
            job_id = entry.value->job_id();
    }
    return job_id;
}

void Shell::save_to(JsonObject& object)
{
    Core::Object::save_to(object);
    object.set("working_directory", cwd);
    object.set("username", username);
    object.set("user_home_path", home);
    object.set("user_id", uid);
    object.set("directory_stack_size", directory_stack.size());
    object.set("cd_history_size", cd_history.size());

    // Jobs.
    JsonArray job_objects;
    for (auto& job_entry : jobs) {
        JsonObject job_object;
        job_object.set("pid", job_entry.value->pid());
        job_object.set("pgid", job_entry.value->pgid());
        job_object.set("running_time", job_entry.value->timer().elapsed());
        job_object.set("command", job_entry.value->cmd());
        job_object.set("is_running_in_background", job_entry.value->is_running_in_background());
        job_objects.append(move(job_object));
    }
    object.set("jobs", move(job_objects));
}
