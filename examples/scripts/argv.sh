# printf reuses the format for each argument; quoting preserves empty words.
printf '%s\n' "$@"
