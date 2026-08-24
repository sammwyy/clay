#!/bin/sh
# Prints lines, words, bytes of $1, one per line - see SKILL.md.
set -eu
wc -l < "$1"
wc -w < "$1"
wc -c < "$1"
