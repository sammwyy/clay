# fish completion for clay
# Generated from src/cli/startup.c and src/shell/shell.c.
complete -c clay -f -n '__fish_use_subcommand' -a shell -d 'Run the native shell interpreter'
complete -c clay -l help -d 'Show command-line help'
complete -c clay -l version -d 'Print the version and exit'
complete -c clay -l no-color -d 'Disable terminal colors'
complete -c clay -l cwd -r -a '(__fish_complete_directories)' -d 'Run the agent from this directory'
complete -c clay -l prompt -r -d 'Send one prompt and exit'
complete -c clay -s p -r -d 'Alias for --prompt'
complete -c clay -n '__fish_seen_subcommand_from shell' -a 'ls cat cp mv rm mkdir touch cd pwd echo true false exit help' -d 'Shell command'
