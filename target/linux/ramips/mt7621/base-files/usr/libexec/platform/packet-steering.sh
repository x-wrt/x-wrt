#!/bin/sh
NPROCS="$(grep -c "^processor.*:" /proc/cpuinfo)"
[ "$NPROCS" -gt 1 ] || exit

PROC_MASK="$(( (1 << $NPROCS) - 1 ))"

find_irq_cpu() {
	local dev="$1"
	local match="$(grep -m 1 "$dev\$" /proc/interrupts)"
	local cpu=0

	[ -n "$match" ] && {
		set -- $match
		shift
		for cur in $(seq 1 $NPROCS); do
			[ "$1" -gt 0 ] && {
				cpu=$(($cur - 1))
				break
			}
			shift
		done
	}

	echo "$cpu"
}

set_hex_val() {
	local file="$1"
	local val="$2"
	val="$(printf %x "$val")"
	[ -n "$DEBUG" ] && echo "$file = $val"
	echo "$val" > "$file"
}

for dev in /sys/class/net/*; do
	[ -d "$dev" ] || continue

	# ignore virtual interfaces
	[ -n "$(ls "${dev}/" | grep '^lower_')" ] && continue
	[ -d "${dev}/device" ] || continue

	device="$(readlink "${dev}/device")"
	device="$(basename "$device")"
	irq_cpu="$(find_irq_cpu "$device")"
	irq_cpu_mask="$((1 << $irq_cpu))"

	for q in ${dev}/queues/tx-*; do
		set_hex_val "$q/xps_cpus" "$PROC_MASK"
	done

	# ignore dsa slave ports for RPS
	subsys="$(readlink "${dev}/device/subsystem")"
	subsys="$(basename "$subsys")"
	[ "$subsys" = "mdio_bus" ] && continue

	for q in ${dev}/queues/rx-*; do
		set_hex_val "$q/rps_cpus" "$PROC_MASK"
	done
done

#set irq smp_affinity for ethernet and usb1
irq=$(echo $(cat /proc/interrupts | grep ethernet | cut -d: -f1))
echo 4 >/proc/irq/${irq}/smp_affinity
irq=$(echo $(cat /proc/interrupts | grep usb1 | cut -d: -f1))
echo 2 >/proc/irq/${irq}/smp_affinity

test -e /sys/class/net/usb0/queues/rx-0/rps_cpus && echo 8 > /sys/class/net/usb0/queues/rx-0/rps_cpus
test -e /sys/class/net/wwan0/queues/rx-0/rps_cpus && echo 8 > /sys/class/net/wwan0/queues/rx-0/rps_cpus
test -e /sys/class/net/wwan0_1/queues/rx-0/rps_cpus && echo 8 > /sys/class/net/wwan0_1/queues/rx-0/rps_cpus
