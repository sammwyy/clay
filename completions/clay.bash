# bash completion for clay
# Generated from the command-line options in src/cli/startup.c and the
# interactive command registry in src/commands/register.c.
_clay() {
    local cur prev
    COMPREPLY=()
    cur=${COMP_WORDS[COMP_CWORD]}
    prev=${COMP_WORDS[COMP_CWORD-1]}

    if [[ ${COMP_CWORD} -eq 1 && ${cur} != -* ]]; then
        COMPREPLY=( $(compgen -W 'shell' -- "${cur}") )
        return 0
    fi

    if [[ ${prev} == --cwd || ${prev} == --prompt || ${prev} == -p ]]; then
        return 0
    fi

    local options='--help --version --no-color --cwd --prompt -p'
    if [[ ${COMP_WORDS[1]} == shell ]]; then
        options+=' ls cat cp mv rm mkdir touch cd pwd echo true false exit help'
    fi
    COMPREPLY=( $(compgen -W "${options}" -- "${cur}") )
}
complete -F _clay clay
