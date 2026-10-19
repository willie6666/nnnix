# Count on the terminal (including the LCD shell). Stop with Ctrl-C.
set COUNT 0
while [ "$COUNT" -lt 5 ]; do
    echo "count: $COUNT"
    inc COUNT
    sleep 1
done
echo finished
unset COUNT
