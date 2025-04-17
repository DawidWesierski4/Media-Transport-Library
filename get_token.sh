#!/bin/bash



JSON_TOKEN=$(curl "https://matrox.sed-vsval.intel.com/login" \
-H 'Accept: */*' \
-H 'Accept-Language: en-US,en;q=0.9,pl;q=0.8' \
-H 'Connection: keep-alive' \
-H 'Content-Type: application/json' \
-H "Origin: https://matrox.sed-vsval.intel.com" \
-H "Referer: https://matrox.sed-vsval.intel.com/login" \
-H 'User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/134.0.0.0 Safari/537.36' \
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