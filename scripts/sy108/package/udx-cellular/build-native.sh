#!/bin/sh
set -eu
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
OUT=${1:?output directory}
JSON_LIBRARY=${2:?OpenWrt libjson-c library or linker argument}
CC=${CC:-musl-gcc}
CFLAGS=${CFLAGS:--Os -Wall -Wextra -Werror}
JSON_INCLUDE=${JSON_INCLUDE:-/usr/include}
SOURCE=${UDX_CELLULAR_SOURCE:-$HERE/src/native}
mkdir -p "$OUT/.cellular-objects"
OBJECTS=$OUT/.cellular-objects
for name in common digest config format sms_pdu transport data receipts sms_history apn identity operators radio advanced modes info debug telemetry controller service network bus-client client quiesce; do
    "$CC" -I"$SOURCE" ${CPPFLAGS:-} $CFLAGS -std=c11 -I"$JSON_INCLUDE" -c "$SOURCE/$name.c" -o "$OBJECTS/$name.o"
done
service_objects=''
for name in common digest config format sms_pdu transport data receipts sms_history apn identity operators radio advanced modes info debug telemetry controller service network; do
    service_objects="$service_objects $OBJECTS/$name.o"
done
"$CC" $CFLAGS $service_objects ${LDFLAGS:-} "$JSON_LIBRARY" -luci -lubus -lubox -lblobmsg_json -lcrypto -pthread -o "$OUT/udx-cellulard"
"$CC" $CFLAGS "$OBJECTS/client.o" "$OBJECTS/common.o" "$OBJECTS/bus-client.o" ${LDFLAGS:-} "$JSON_LIBRARY" -luci -lubus -lubox -lblobmsg_json -lcrypto -pthread -o "$OUT/udx-cellular-control"
"$CC" $CFLAGS "$OBJECTS/quiesce.o" ${LDFLAGS:-} -lubus -lubox -o "$OUT/udx-system-quiesce"
