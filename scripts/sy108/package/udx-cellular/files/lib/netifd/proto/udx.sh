#!/bin/sh
. /lib/functions.sh
. /lib/netifd/netifd-proto.sh
init_proto "$@"
proto_udx_init_config() {
 no_device=1
 available=1
 no_proto_task=1
 renew_handler=1
 proto_config_add_string device
 proto_config_add_int metric
}
proto_udx_setup() {
 local interface="$1" device metric result accepted
 json_get_vars device metric
 [ "$interface" = wan ] && [ "$device" = wwan0 ] || {
  proto_notify_error "$interface" INVALID_DEVICE
  proto_block_restart "$interface"
  return 1
 }
 ubus -t 180 wait_for udx.cellular || return 1
 json_init
 json_add_string interface "$interface"
 json_add_string device "$device"
 json_add_int metric "${metric:-50}"
 result=$(ubus call udx.cellular network_setup "$(json_dump)") || return 1
 json_load "$result" || return 1
 json_get_var accepted accepted
 [ "$accepted" = 1 ] || {
  proto_notify_error "$interface" SETUP_REJECTED
  return 1
 }
}
proto_udx_renew() { proto_udx_setup "$@"; }
proto_udx_teardown() {
 local interface="$1"
 json_init
 json_add_string interface "$interface"
 ubus call udx.cellular network_teardown "$(json_dump)" >/dev/null
 proto_init_update "*" 0
 proto_send_update "$interface"
}
add_protocol udx
