#!/bin/bash
script="$1"
expect="$2"

diff "$expect" -u <("$script" | sed -e 's,\(https://.*.rbi#L\)[0-9]*,\1__LINE__,')
