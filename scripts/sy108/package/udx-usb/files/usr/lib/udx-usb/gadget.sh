#!/bin/sh
# Shared implementation; tests source this library and replace only filesystem
# primitives. Production entry points always use the fixed paths below.
UDX_UDC=29100000.usb
UDX_CF=/sys/kernel/config/usb_gadget
UDX_G=$UDX_CF/udx-usb
UDX_RUN=/run/udx-usb
UDX_UDC_CLASS=/sys/class/udc
UDX_NET_CLASS=/sys/class/net
UDX_SHADOW=/etc/shadow
UDX_IDENTITY_CMDLINE=/proc/cmdline
UDX_AFFINITY_PROC=/proc
UDX_AFFINITY_SYS=/sys

g_error() { echo "udx-usb: $*" >&2; }
g_read() { cat "$1"; }
g_write() { printf '%s\n' "$2" > "$1"; }
g_mkdir() { mkdir "$1"; }
g_rmdir() { rmdir "$1"; }
g_link() { ln -s "$1" "$2"; }
g_unlink() { rm "$1"; }

# Keep the AP Ethernet consumer and completion paths on CPU1, and its
# receive-buffer refill on CPU0. No application or modem thread is pinned.
g_affinity_task() {
	local pid=$1 name=$2
	[ "$(cat "$UDX_AFFINITY_PROC/$pid/comm" 2>/dev/null)" = "$name" ] &&
		grep -Eq '^Kthread:[[:space:]]+1$' "$UDX_AFFINITY_PROC/$pid/status"
}

g_affinity_mask() {
	awk '$1=="Cpus_allowed:" {print $2}' "$UDX_AFFINITY_PROC/$1/status"
}

g_affinity_irq() {
	awk '$NF=="sprd,sipa" {gsub(":","",$1);print $1}' "$UDX_AFFINITY_PROC/interrupts"
}

g_affinity_restore() {
	local kind id name stamp previous assigned current failed=0
	[ -f "$UDX_RUN/affinity" ] || return 0
	while read -r kind id name stamp previous assigned; do
		case "$kind" in
			task)
				g_affinity_task "$id" "$name" || continue
				[ "$(awk '{print $22}' "$UDX_AFFINITY_PROC/$id/stat")" = "$stamp" ] || continue
				current=$(g_affinity_mask "$id")
				[ "$current" = "$assigned" ] || continue
				taskset -p "$previous" "$id" >/dev/null || failed=1
				;;
			irq)
				[ "$(g_affinity_irq)" = "$id" ] || continue
				current=$(cat "$UDX_AFFINITY_PROC/irq/$id/smp_affinity_list")
				[ "$current" = "$assigned" ] || continue
				printf '%s\n' "$previous" > "$UDX_AFFINITY_PROC/irq/$id/smp_affinity_list" || failed=1
				;;
		esac
	done < "$UDX_RUN/affinity"
	[ "$failed" = 0 ] && rm -f "$UDX_RUN/affinity"
	return "$failed"
}

g_affinity_apply() {
	local path pid name mask previous stamp irq count=0 kind id assigned
	[ "$PAMU3" = 1 ] || return 0
	[ "$(cat "$UDX_AFFINITY_SYS/devices/system/cpu/online" 2>/dev/null)" = 0-1 ] || return 0
	tr '\000' '\n' < "$UDX_AFFINITY_SYS/firmware/devicetree/base/compatible" |
		grep -qx 'sprd,udx710' || return 0
	command -v taskset >/dev/null || return 0
	[ ! -e "$UDX_RUN/affinity" ] || return 0
	: > "$UDX_RUN/affinity.new" || return 1
	for path in "$UDX_AFFINITY_PROC"/[0-9]*/comm; do
		name=$(cat "$path" 2>/dev/null) || continue
		case "$name" in
			sipa-send-2|sipa-recv-2) mask=2;;
			sipa-free-2|sipa-fill-2) mask=1;;
			*) continue;;
		esac
		pid=${path%/comm}; pid=${pid##*/}
		g_affinity_task "$pid" "$name" || continue
		previous=$(g_affinity_mask "$pid")
		stamp=$(awk '{print $22}' "$UDX_AFFINITY_PROC/$pid/stat")
		[ -n "$previous" ] && [ -n "$stamp" ] || continue
		printf 'task %s %s %s %s %s\n' "$pid" "$name" "$stamp" "$previous" "$mask" >> "$UDX_RUN/affinity.new"
		count=$((count + 1))
	done
	irq=$(g_affinity_irq)
	case "$irq" in ''|*[!0-9]*) rm -f "$UDX_RUN/affinity.new"; return 1;; esac
	[ "$count" = 4 ] || { rm -f "$UDX_RUN/affinity.new"; return 1; }
	previous=$(cat "$UDX_AFFINITY_PROC/irq/$irq/smp_affinity_list") || return 1
	printf 'irq %s sprd,sipa - %s 1\n' "$irq" "$previous" >> "$UDX_RUN/affinity.new"
	mv "$UDX_RUN/affinity.new" "$UDX_RUN/affinity" || return 1
	while read -r kind id name stamp previous assigned; do
		case "$kind" in
			task) taskset -p "$assigned" "$id" >/dev/null && continue;;
			irq) printf '%s\n' "$assigned" > "$UDX_AFFINITY_PROC/irq/$id/smp_affinity_list" && continue;;
		esac
		g_affinity_restore
		return 1
	done < "$UDX_RUN/affinity"
}

g_factory_serial() {
	local answer
	IDENTITY_REASON=cmdline-unavailable
	[ -r "$UDX_IDENTITY_CMDLINE" ] || return 1
	answer=$(LC_ALL=C awk '
	 {for(i=1;i<=NF;i++) if($i ~ /^androidboot.serialno=/) {n++;v=substr($i,22)}}
	 END {
	  if(n==0) print "missing-serial";
	  else if(n!=1) print "duplicate-serial";
	  else if(length(v)<6 || length(v)>64 || v !~ /^[A-Za-z0-9._-]+$/) print "invalid-serial";
	  else if(toupper(v)=="0123456789ABCDEF" || toupper(v) ~ /^[0F]+$/ ||
	          toupper(v) ~ /^(UNKNOWN|UNSET|NONE|DEFAULT|SERIALNUMBER|ANDROID)$/) print "placeholder-serial";
	  else print "ok:" v;
	 }' "$UDX_IDENTITY_CMDLINE") || return 1
	case "$answer" in
		ok:*) FACTORY_SERIAL=${answer#ok:}; IDENTITY_REASON=none; return 0;;
		*) IDENTITY_REASON=$answer; return 1;;
	esac
}

g_identity_hash() {
	local value
	value=$({ printf '%s\000' "$1"; printf '%s' "$FACTORY_SERIAL"; } | sha256sum) || return 1
	value=${value%% *}
	[ "${#value}" = 64 ] || return 1
	case "$value" in *[!0-9a-f]*) return 1;; esac
	printf '%s\n' "$value"
}

g_identity_mac() {
	local rest=$1 role=$2 a b c d e f
	a=${rest%"${rest#??}"}; rest=${rest#??}
	b=${rest%"${rest#??}"}; rest=${rest#??}
	c=${rest%"${rest#??}"}; rest=${rest#??}
	d=${rest%"${rest#??}"}; rest=${rest#??}
	e=${rest%"${rest#??}"}; rest=${rest#??}
	f=${rest%"${rest#??}"}
	# Local unicast, preserving the existing device/host role bits.
	printf '%02x:%s:%s:%s:%s:%02x\n' "$(((0x$a & 252) | 2))" "$b" "$c" "$d" "$e" "$(((0x$f & 252) | role))"
}

g_identity_defaults() {
	local serial_hash mac_hash
	AUTO_SERIAL=sy108-usb
	AUTO_DEV_MAC=02:71:04:10:00:01 AUTO_HOST_MAC=02:71:04:10:00:02
	IDENTITY_SOURCE=diagnostic-fallback
	g_factory_serial || return 0
	IDENTITY_REASON=hash-unavailable
	serial_hash=$(g_identity_hash udx710-usb-serial-v1) || return 0
	mac_hash=$(g_identity_hash udx710-usb-mac-v1) || return 0
	AUTO_SERIAL=udx710-$(printf '%s' "$serial_hash" | cut -c1-16)
	AUTO_DEV_MAC=$(g_identity_mac "$mac_hash" 0)
	AUTO_HOST_MAC=$(g_identity_mac "$mac_hash" 1)
	IDENTITY_SOURCE=factory-serial IDENTITY_REASON=none
}

# /proc/uptime is monotonic and does not depend on RTC/NTP or logd.
g_boot_clock() {
	local value rest fraction
	read -r value rest < /proc/uptime || return 1
	fraction=${value#*.}
	fraction=${fraction#0}
	G_NOW_CS=$(( ${value%.*} * 100 + ${fraction:-0} ))
}

g_boot_mark() {
	g_boot_clock || return 0
	local message="udx-usb: uptime_cs=$G_NOW_CS $*"
	printf '%s\n' "$message" >> "$UDX_RUN/boot-timing"
	[ ! -w /dev/kmsg ] || printf '<14>%s\n' "$message" > /dev/kmsg || true
}

g_load_config() {
	PROFILE=$(uci -q get udx-usb.main.profile) || return 1
	ACM=$(uci -q get udx-usb.main.acm) || ACM=0
	SERIAL=$(uci -q get udx-usb.main.serial) || SERIAL=auto
	VID=$(uci -q get udx-usb.main.vid) || return 1
	DEV_MAC=$(uci -q get udx-usb.main.dev_mac) || DEV_MAC=auto
	HOST_MAC=$(uci -q get udx-usb.main.host_mac) || HOST_MAC=auto
	IDENTITY_SOURCE=uci-explicit IDENTITY_REASON=none
	case " $SERIAL $DEV_MAC $HOST_MAC " in
		*' auto '*) g_identity_defaults;;
	esac
	[ "$SERIAL" != auto ] || SERIAL=$AUTO_SERIAL
	[ "$DEV_MAC" != auto ] || DEV_MAC=$AUTO_DEV_MAC
	[ "$HOST_MAC" != auto ] || HOST_MAC=$AUTO_HOST_MAC
}

g_profile() {
	case "$PROFILE" in
		ncm) FUNCTIONS=ncm; PID=0x0105; PAMU3=1 ;;
		ncm-software) FUNCTIONS=ncm; PID=0x0105; PAMU3=0 ;;
		*) g_error 'profile must be ncm or ncm-software'; return 64 ;;
	esac
	case "$ACM" in
		0) ;;
		1) PID=$(printf '0x%04x' "$((PID + 16))") ;;
		*) g_error 'acm must be 0 or 1'; return 64 ;;
	esac
}

g_validate() {
	g_profile || return $?
	case "$SERIAL" in ''|*[!A-Za-z0-9._-]*) g_error 'invalid serial'; return 64;; esac
	[ "${#SERIAL}" -le 64 ] || return 64
	printf '%s\n' "$VID" | grep -Eq '^0x[0-9a-fA-F]{4}$' || return 64
	DEV_MAC=$(printf '%s' "$DEV_MAC" | tr A-F a-f)
	HOST_MAC=$(printf '%s' "$HOST_MAC" | tr A-F a-f)
	local mac
	for mac in "$DEV_MAC" "$HOST_MAC"; do
		printf '%s\n' "$mac" | grep -Eq '^([0-9a-f]{2}:){5}[0-9a-f]{2}$' || { g_error 'invalid MAC address'; return 64; }
		[ "$((0x${mac%%:*} & 3))" = 2 ] || { g_error 'MAC must be local unicast'; return 64; }
	done
	[ "$DEV_MAC" != "$HOST_MAC" ] || {
		g_error 'device and host MAC addresses must be distinct'; return 64;
	}
}

g_console_getty() {
	if [ -x /usr/sbin/agetty ]; then
		echo /usr/sbin/agetty
	elif [ -x /sbin/getty ]; then
		echo /sbin/getty
	else
		return 1
	fi
}

g_console_reason() {
	local hash
	hash=$(awk -F: '$1 == "root" {print $2; exit}' "$UDX_SHADOW" 2>/dev/null)
	case "$hash" in ''|'!'*|'*'*) echo password-required; return 1;; esac
	g_console_getty >/dev/null && [ -x /bin/login ] || { echo login-tools-missing; return 1; }
	echo ready
}

g_preflight() {
	g_validate || return $?
	[ -d "$UDX_UDC_CLASS/$UDX_UDC" ] || { g_error 'expected Device controller 29100000.usb is absent'; return 1; }
	[ -d "$UDX_CF" ] || { g_error 'configfs usb_gadget is unavailable'; return 1; }
	if [ "$ACM" = 1 ]; then
		local reason
		reason=$(g_console_reason) || { g_error "ACM disabled: $reason"; return 1; }
	fi
	local other owner
	for other in "$UDX_CF"/*/UDC; do
		[ -e "$other" ] || continue
		[ "$other" = "$UDX_G/UDC" ] && continue
		owner=$(g_read "$other") || return 1
		[ "$owner" != "$UDX_UDC" ] || { g_error 'UDC belongs to another gadget'; return 1; }
	done
	if [ -d "$UDX_G" ] && [ ! -f "$UDX_RUN/owned" ]; then
		g_error 'existing udx-usb gadget has no ownership record'; return 1
	fi
}

g_lock() {
	mkdir -p "$UDX_RUN" || return 1
	umask 077
	exec 9>"$UDX_RUN/control.lock" || return 1
	if [ "${1:-}" = wait ]; then flock -w 10 -x 9; else flock -n -x 9; fi || { g_error 'another USB operation is active'; return 75; }
}

g_net_function() {
	local dir=$UDX_G/functions/ncm.usb0
	g_mkdir "$dir" || return 1
	g_write "$dir/ifname" 'uncm%d' || return 1
	g_write "$dir/dev_addr" "$DEV_MAC" || return 1
	g_write "$dir/host_addr" "$HOST_MAC" || return 1
	g_write "$dir/max_segment_size" 1514 || return 1
	g_write "$dir/pamu3" "$PAMU3" || return 1
	# Windows UsbNcm.sys matches MS_COMP_WINNCM. WINUSB would select
	# Winusb.sys and would not expose this function as a network adapter.
	g_write "$dir/os_desc/interface.ncm/compatible_id" WINNCM || return 1
	g_write "$dir/os_desc/interface.ncm/sub_compatible_id" '' || return 1
	g_link "$dir" "$UDX_G/configs/c.1/ncm.usb0"
}

g_create() {
	local kind actual expected
	g_boot_mark "event=identity source=${IDENTITY_SOURCE:-uci-explicit} reason=${IDENTITY_REASON:-none}"
	[ "${IDENTITY_SOURCE:-}" != diagnostic-fallback ] || g_error "factory serial unavailable ($IDENTITY_REASON); using generic diagnostic identity"
	[ ! -e "$UDX_G" ] || return 1
	# Refuse conflicting names before changing the intended gadget.
	for expected in uncm0; do
		[ ! -e "$UDX_NET_CLASS/$expected" ] || { g_error "interface $expected already exists"; return 1; }
	done
	g_mkdir "$UDX_G" || return 1
	: > "$UDX_RUN/owned" || return 1
	g_write "$UDX_G/idVendor" "$VID" || return 1
	g_write "$UDX_G/idProduct" "$PID" || return 1
	g_write "$UDX_G/bcdUSB" 0x0200 || return 1
	g_write "$UDX_G/bcdDevice" 0x0100 || return 1
	g_write "$UDX_G/max_speed" super-speed || return 1
	g_write "$UDX_G/bDeviceClass" 0xef || return 1
	g_write "$UDX_G/bDeviceSubClass" 0x02 || return 1
	g_write "$UDX_G/bDeviceProtocol" 0x01 || return 1
	g_mkdir "$UDX_G/strings/0x409" || return 1
	g_write "$UDX_G/strings/0x409/manufacturer" UDX710 || return 1
	g_write "$UDX_G/strings/0x409/product" "SY108 $PROFILE management" || return 1
	g_write "$UDX_G/strings/0x409/serialnumber" "$SERIAL" || return 1
	g_mkdir "$UDX_G/configs/c.1" || return 1
	g_write "$UDX_G/configs/c.1/bmAttributes" 0x80 || return 1
	g_write "$UDX_G/configs/c.1/MaxPower" 500 || return 1
	g_mkdir "$UDX_G/configs/c.1/strings/0x409" || return 1
	g_write "$UDX_G/configs/c.1/strings/0x409/configuration" "$PROFILE management" || return 1
	for kind in $FUNCTIONS; do g_net_function "$kind" || return 1; done
	g_write "$UDX_G/os_desc/use" 1 || return 1
	g_write "$UDX_G/os_desc/b_vendor_code" 0xcd || return 1
	g_write "$UDX_G/os_desc/qw_sign" MSFT100 || return 1
	g_link "$UDX_G/configs/c.1" "$UDX_G/os_desc/c.1" || return 1
	if [ "$ACM" = 1 ]; then
		g_mkdir "$UDX_G/functions/acm.maint" || return 1
		[ "$(g_read "$UDX_G/functions/acm.maint/port_num")" = 0 ] || { g_error 'ttyGS0 is already allocated'; return 1; }
		if [ -e "$UDX_G/functions/acm.maint/console" ]; then
			g_write "$UDX_G/functions/acm.maint/console" 0 || return 1
		fi
		g_link "$UDX_G/functions/acm.maint" "$UDX_G/configs/c.1/acm.maint" || return 1
	fi
	g_affinity_apply || g_error 'CPU affinity unavailable; retaining default scheduling'
	g_boot_mark 'event=udc-bind-begin'
	# Bind once after the complete, deterministic descriptor tree exists.
	g_write "$UDX_G/UDC" "$UDX_UDC" || return 1
	[ "$(g_read "$UDX_G/UDC")" = "$UDX_UDC" ] || return 1
	g_boot_mark 'event=udc-bind-done'
	for kind in $FUNCTIONS; do
		expected=uncm0
		actual=$(g_read "$UDX_G/functions/$kind.usb0/ifname") || return 1
		[ "$actual" = "$expected" ] || { g_error "unexpected interface $actual"; return 1; }
	done
}

g_destroy() {
	local path owner
	[ -d "$UDX_G" ] || { g_affinity_restore || true; rm -f "$UDX_RUN/owned" "$UDX_RUN/applied"; return 0; }
	[ -f "$UDX_RUN/owned" ] || { g_error 'refusing to remove an unowned gadget'; return 1; }
	owner=$(g_read "$UDX_G/UDC") || return 1
	case "$owner" in
		'') ;; # A failed build may never have bound; kernel unbind would return ENODEV.
		"$UDX_UDC") g_write "$UDX_G/UDC" '' || return 1;;
		*) g_error 'owned gadget is bound to an unexpected controller'; return 1;;
	esac
	for path in "$UDX_G/os_desc/c.1" "$UDX_G/configs/c.1/acm.maint" \
		"$UDX_G/configs/c.1/ncm.usb0"; do
		[ ! -L "$path" ] || g_unlink "$path" || return 1
	done
	for path in "$UDX_G/configs/c.1/strings/0x409" "$UDX_G/configs/c.1" \
		"$UDX_G/functions/acm.maint" "$UDX_G/functions/ncm.usb0" \
		"$UDX_G/strings/0x409"; do
		[ ! -d "$path" ] || g_rmdir "$path" || return 1
	done
	g_rmdir "$UDX_G" || return 1
	g_affinity_restore || g_error 'could not restore all CPU affinities'
	rm -f "$UDX_RUN/owned" "$UDX_RUN/applied"
}

g_state_save() {
	{
		printf 'profile=%s\nacm=%s\nserial=%s\nvid=%s\n' "$PROFILE" "$ACM" "$SERIAL" "$VID"
		printf 'dev_mac=%s\nhost_mac=%s\n' \
			"$DEV_MAC" "$HOST_MAC"
		printf 'identity_source=%s\nidentity_reason=%s\n' "${IDENTITY_SOURCE:-uci-explicit}" "${IDENTITY_REASON:-none}"
	} > "$UDX_RUN/applied.new" && mv "$UDX_RUN/applied.new" "$UDX_RUN/applied"
}

g_state_load() {
	local key value
	[ -f "$1" ] || return 1
	IDENTITY_SOURCE=uci-explicit IDENTITY_REASON=none
	while IFS='=' read -r key value; do
		case "$key" in
			profile) PROFILE=$value;; acm) ACM=$value;; serial) SERIAL=$value;; vid) VID=$value;;
			dev_mac) DEV_MAC=$value;; host_mac) HOST_MAC=$value;;
			identity_source) IDENTITY_SOURCE=$value;; identity_reason) IDENTITY_REASON=$value;;
			*) return 1;;
		esac
	done < "$1"
	g_validate
}

# Called under the same control lock as profile changes and teardown.
g_reset() {
	[ -d "$UDX_G" ] || return 0
	[ "$(g_read "$UDX_G/UDC")" = "$UDX_UDC" ] || {
		g_error 'MCU reset requires the managed gadget to be bound'; return 1;
	}
	local result=0
	g_write "$UDX_UDC_CLASS/$UDX_UDC/soft_connect" disconnect || result=1
	sleep 0.5 || result=1
	# Preserve one connect attempt even if the disconnect reported an error.
	g_write "$UDX_UDC_CLASS/$UDX_UDC/soft_connect" connect || result=1
	return "$result"
}

g_status() {
	local reason bound=none state=absent speed=unknown
	[ ! -f "$UDX_RUN/applied" ] || cat "$UDX_RUN/applied"
	[ ! -f "$UDX_G/UDC" ] || bound=$(g_read "$UDX_G/UDC")
	[ ! -f "$UDX_UDC_CLASS/$UDX_UDC/state" ] || state=$(g_read "$UDX_UDC_CLASS/$UDX_UDC/state")
	[ ! -f "$UDX_UDC_CLASS/$UDX_UDC/current_speed" ] || speed=$(g_read "$UDX_UDC_CLASS/$UDX_UDC/current_speed")
	printf 'udc=%s\nstate=%s\nspeed=%s\n' "${bound:-none}" "$state" "$speed"
	reason=$(g_console_reason) || true
	printf 'console_auth=%s\n' "$reason"
	[ ! -f "$UDX_RUN/pending" ] || { printf 'pending='; cat "$UDX_RUN/pending"; }
	[ ! -f "$UDX_RUN/last-result" ] || cat "$UDX_RUN/last-result"
	local readiness
	readiness=$(/usr/libexec/udx-usb-ready 2>/dev/null) || true
	printf '%s\n' "${readiness:-dhcp_readiness=not-ready
dhcp_reason=query-failed}"
}
