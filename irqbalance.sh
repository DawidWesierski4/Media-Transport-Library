#! /bin/bash
  systemctl stop irqbalance
    pf_bdfs=($(lspci | grep "Ethernet controller: Intel.*E810" | awk '{print "0000:"$1}'))

    for pf in "${pf_bdfs[@]}"; do
        nic_iface="$(ls "/sys/bus/pci/devices/$pf/net/")";
        nic_state="$(ip -json link show dev "${nic_iface}" | jq '.[].operstate' -r)";
        nic_numa_node="$(cat /sys/bus/pci/devices/$pf/numa_node)"

        if [[ "${nic_state}" == "UP" ]]; then
            PF_NIC_BDFS+=("${pf}")
            PF_NIC_IFACE+=("${nic_iface}")

            nicctl create_tvf "${pf}"

            for irq in $(cat /sys/bus/pci/devices/$pf/msi_irqs/*); do
                echo "${NUMA_CORES["${nic_numa_node}"]}" > /proc/irq/$irq/smp_affinity_list
            done
            /opt/intel/drivers/ice/1.14.9/scripts/set_irq_affinity -X "${NUMA_CORES["${nic_numa_node}"]}" "${nic_iface}"

            ethtool -G $nic_iface rx 4096 tx 4096
            ethtool -C $nic_iface adaptive-rx off adaptive-tx off rx-usecs-high 50 rx-usecs 50 tx-usecs 50
            ethtool -s $nic_iface speed 100000 duplex full autoneg off
            ethtool -N $nic_iface rx-flow-hash udp4 sdfn
            ethtool -N $nic_iface rx-flow-hash udp6 sdfn
        fi
    done