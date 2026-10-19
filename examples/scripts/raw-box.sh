# Generate all pixels in nsh: a filled rectangle, x=24..103, y=16..47.
# Optional first argument: output bitmap path. No precomputed image is needed.
if [ "$#" -gt 0 ]; then
    set OUTPUT "$1"
else
    set OUTPUT /home/.nsh-box.raw
fi
printf '' > "$OUTPUT"
set PAGE 0
while [ "$PAGE" -lt 8 ]; do
    set X 0
    set N 0
    set DATA ''
    while [ "$X" -lt 128 ]; do
        set B '\0'
        if [ "$PAGE" -ge 2 ]; then
            if [ "$PAGE" -le 5 ]; then
                if [ "$X" -ge 24 ]; then
                    if [ "$X" -le 103 ]; then
                        set B '\xff'
                    fi
                fi
            fi
        fi
        set DATA "$DATA$B"
        inc N
        inc X
        if [ "$N" -eq 32 ]; then
            printf '%b' "$DATA" >> "$OUTPUT"
            set DATA ''
            set N 0
        fi
    done
    inc PAGE
done
unset PAGE
unset X
unset N
unset DATA
unset B
echo "Rectangle ready; Ctrl-D, Ctrl-C or INT to finish."
cat "$OUTPUT" /dev/tty > /dev/lcd
unset OUTPUT
