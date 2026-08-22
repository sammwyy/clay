# zsh completion for clay. Install or source this file as _clay.
# Generated from src/cli/startup.c and src/commands/register.c.
#compdef clay

_clay() {
    local -a args
    args=(
        '--help[Show command-line help]'
        '--version[Print the version and exit]'
        '--no-color[Disable terminal colors]'
        '--cwd[Run the agent from this directory]:directory:_directories'
        '--prompt[Send one prompt and exit]:prompt:'
        '-p[Alias for --prompt]:prompt:'
        '1:subcommand:(shell)'
    )
    if [[ ${words[2]} == shell ]]; then
        args+=(
            '*:shell command:(ls cat cp mv rm mkdir touch cd pwd echo true false exit help)'
        )
    fi
    _arguments -s $args
}

_clay "$@"
