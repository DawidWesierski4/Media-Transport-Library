


NUMA_CORES="28-55"


systemctl stop irqbalance
cpupower idle-set -D0

pf=("0000:4b:00.1", "0000:b1:00.1")
pf_names=("ens785f1np1", "ens801f1np1")


for pf in "${pf[@]}"; do
    for irq in $(ls /sys/bus/pci/devices/$pf/msi_irqs); do
        if [ -f "/proc/irq/$irq/smp_affinity_list" ]; then
            echo "Setting IRQ $irq to NUMA cores $NUMA_CORES"
            echo "${NUMA_CORES}" > /proc/irq/$irq/smp_affinity_list
        else
            echo "IRQ $irq does not support smp_affinity_list, skipping..."
            continue
        fi
    done
    ./"$(dirname $0)"/irq_2.sh -X "${NUMA_CORES}" "${pf_names[@]}"
done
