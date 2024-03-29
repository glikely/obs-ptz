#!/bin/bash
dirty=$(git ls-files --modified)

set +x
if [[ $dirty ]]; then
	echo "================================="
    echo "Files were not formatted properly"
    echo "$dirty"
    echo "================================="
    git diff
    exit 1
fi
