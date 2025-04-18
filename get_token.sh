#!/bin/bash


http_proxy=
JSON_TOKEN=$(curl "https://matrox.sed-vsval.intel.com/login" \
-H 'Content-Type: application/json' \
-H "Origin: https://matrox.sed-vsval.intel.com" \
-H "Referer: https://matrox.sed-vsval.intel.com/login" \
--data-raw "{username:admin,password:admin}" \
--insecure)

TOKEN=$(echo $JSON_TOKEN | jq -r '.content.token')

echo $JSON_TOKEN

echo
echo
echo $TOKEN
exit

curl -X PUT \
-H "Authorization: Bearer $TOKEN" \
-H "Content-Type: multipart/form-data" \
-F "pcap=@${TCP_FILE_1};type=application/vnd.tcpdump.pcap" \
"http://${IP_EBU}/api/pcap"