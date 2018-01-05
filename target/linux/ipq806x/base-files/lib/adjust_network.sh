#!/bin/sh
# this scripts is used for adjust cpu's choice of interrupts.
#

################################################
# Adjust smp_affinity of edma
# Globals:
#    None
# Arguments:
#    None
# Returns:
#    None
# Remark:
#    execute only once on start-up.
################################################
adjust_edma_smp_affinity() {
	local cpu=0
	local tx_irq_num

	for tx_num in `seq 0 1 15` ; do
		case $((tx_num/4)) in
			0) cpu=4;;
			1) cpu=8;;
			2) cpu=1;;
			3) cpu=2;;
		esac

		tx_irq_num=`grep -m1 edma_eth_tx$tx_num /proc/interrupts | cut -d ':' -f 1 | tail -n1 | tr -d ' '`
		[ -n "$tx_irq_num" ] && echo $cpu > /proc/irq/$tx_irq_num/smp_affinity
	done

	for rx_num in `seq 0 1 7` ; do
		case $((rx_num/2)) in
			0) cpu=1;;
			1) cpu=2;;
			2) cpu=4;;
			3) cpu=8;;
		esac

		rx_irq_num=`grep -m1 edma_eth_rx$rx_num /proc/interrupts | cut -d ':' -f 1 | tail -n1 | tr -d ' '`
		[ -n "$rx_irq_num" ] && echo $cpu > /proc/irq/$rx_irq_num/smp_affinity
	done
}

################################################
# Adjust smp_affinity of ath10k for 2G and 5G
# Globals:
#    None
# Arguments:
#    None
# Returns:
#    None
# Remark:
#    execute only once on start-up.
################################################
adjust_radio_smp_affinity() {
	local irq_radio0=`grep -E -m1 'ath10k' /proc/interrupts | cut -d ':' -f 1 | tail -n1 | tr -d ' '`
	local irq_radio1=`grep -E -m2 'ath10k' /proc/interrupts | cut -d ':' -f 1 | tail -n1 | tr -d ' '`
	local board="`cat /tmp/sysinfo/board_name 2>/dev/null`"

	# Enable smp_affinity for ath10k driver
	if [ -n "$irq_radio0" ]; then
		[ -n "$board" ] || board="generic"

		case "$board" in
			rt-acrh17|\
			ap-dk0*)
				echo 4 > /proc/irq/$irq_radio0/smp_affinity
				[ -n "$irq_radio1" ] && {
				echo 8 > /proc/irq/$irq_radio1/smp_affinity
				}
			;;
			ap148*)
				echo 2 > /proc/irq/$irq_radio0/smp_affinity
			;;
		esac
	fi
}

################################################
# Adjust queue of eth
# Globals:
#    None
# Arguments:
#    None
# Returns:
#    None
# Remark:
#    Each network reboot needs to be executed.
################################################
adjust_eth_queue() {
	for eth_interface in 0 1
	do
		if [ -d /sys/class/net/eth$eth_interface ] ; then
			for tx_queue in 0 1 2 3
			do
				val=$(( 2 ** $tx_queue))
				echo $val > /sys/class/net/eth$eth_interface/queues/tx-$tx_queue/xps_cpus
				echo $val > /sys/class/net/eth$eth_interface/queues/rx-$tx_queue/rps_cpus
			done
		fi
	done

	for eth_interface in 0 1
	do
		if [ -d /sys/class/net/eth$eth_interface ] ; then
			for rx_queue in 0 1 2 3
			do
				echo 256 > /sys/class/net/eth$eth_interface/queues/rx-$rx_queue/rps_flow_cnt
			done
		fi
	done

	echo 1024 > /proc/sys/net/core/rps_sock_flow_entries

	for eth_interface in 0 1 2 3 4 ; do
		[ -d /sys/class/net/eth$eth_interface ] && ethtool -K eth${eth_interface} gro off
	done
}
