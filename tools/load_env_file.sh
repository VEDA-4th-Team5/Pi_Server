#!/usr/bin/env bash

# Load simple KEY=VALUE files without using `source`.
# This deliberately accepts CRLF so a file copied from Windows cannot inject
# a trailing carriage return into an environment variable or shell command.
load_env_file() {
    local file_path="${1:?environment file path is required}"
    [[ -f "$file_path" ]] || return 0

    local line key value
    while IFS= read -r line || [[ -n "$line" ]]; do
        line="${line%$'\r'}"
        line="${line#${line%%[![:space:]]*}}"
        [[ -z "$line" || "${line:0:1}" == "#" ]] && continue
        [[ "$line" == export\ * ]] && line="${line#export }"
        [[ "$line" == *=* ]] || continue

        key="${line%%=*}"
        value="${line#*=}"
        key="${key//[[:space:]]/}"
        [[ "$key" =~ ^[A-Za-z_][A-Za-z0-9_]*$ ]] || continue
        value="${value%$'\r'}"
        if [[ "$value" == \"*\" && "$value" == *\" ]]; then
            value="${value:1:${#value}-2}"
        elif [[ "$value" == \'*\' && "$value" == *\' ]]; then
            value="${value:1:${#value}-2}"
        fi

        # Empty values in the example file are placeholders. Do not erase a
        # value already supplied by a machine-local file or the shell.
        [[ -n "$value" ]] && export "$key=$value"
    done < "$file_path"
}
