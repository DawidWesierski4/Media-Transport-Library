#!/bin/bash

SCRIPT_NAME=repro_docker.sh

if [ -z $1 ]; then
    REPO_NMB=$(cat logs_${SCRIPT_NAME}/${SCRIPT_NAME}_repo_number)
else
    REPO_NMB=$1
fi
echo "$REPO_NMB"

#!/bin/bash

SCRIPT_NAME="repro_docker.sh"
#REPO_NMB=$(cat logs_${SCRIPT_NAME}/${SCRIPT_NAME}_repo_number)

set -x
for log_file in logs_${SCRIPT_NAME}/repo_${REPO_NMB}.log_*; do
    if [[ -f "$log_file" ]]; then
        echo Processing "$log_file" mismatch
        grep "epoch mismatch" "$log_file"| \
        awk '{print $3, $4, $6, $7}' | \
        sort -k2,2 -k1,1
        echo Processing "$log_file" late
        grep "epoch late" "$log_file"| \
        awk '{print $3, $4, $6, $7}' | \
        sort -k2,2 -k1,1
        echo Processing "$log_file" drop
        grep "epoch drop" "$log_file" | \
        awk '{print $3, $4, $6, $7}'  |  \
        sort -k2,2 -k1,1
    fi
done
