#!/bin/sh

cat > AUTHORS << EOF
Original Author: Grant Likely <grant.likely@secretlab.ca>

Contributions are sorted by their number of commits

Contributors:
EOF
git log --pretty=format:%an HEAD --not template/master | sort | uniq -c | sort -nr | cut -c4- >> AUTHORS
