#!/bin/sh
# gendoc.sh : extract @doc tags from source files into a text document
# Usage: sh gendoc.sh file1.c file2.c ... > output.txt
#
# Recognizes these comment tags:
#   @doc <text>     - general documentation line
#   @param <text>   - parameter description
#   @return <text>  - return value description
#
# Each file's entries are grouped under a filename heading.

for f in "$@"; do
    printed_header=0
    while IFS= read -r line; do
        case "$line" in
        *@doc\ *|*@param\ *|*@return\ *)
            if [ "$printed_header" -eq 0 ]; then
                printf '=== %s ===\n' "$f"
                printed_header=1
            fi
            # strip everything before the tag
            tag="${line#*@}"
            printf '  @%s\n' "$tag"
            ;;
        esac
    done < "$f"
    if [ "$printed_header" -eq 1 ]; then
        printf '\n'
    fi
done
