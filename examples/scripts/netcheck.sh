# Try the computer up to three times. Change HOST for your network.
set HOST 198.18.0.1
set ATTEMPT 0
while [ "$ATTEMPT" -lt 3 ]; do
    inc ATTEMPT
    if ping -c 1 -W 1 "$HOST"; then
        echo "connected to $HOST"
        break
    else
        echo "attempt $ATTEMPT failed"
        sleep 1
    fi
done
unset HOST
unset ATTEMPT
